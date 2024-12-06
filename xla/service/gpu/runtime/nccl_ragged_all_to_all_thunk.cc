/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/runtime/nccl_ragged_all_to_all_thunk.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/node_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "xla/backends/gpu/collectives/gpu_clique_key.h"
#include "xla/backends/gpu/collectives/gpu_collectives.h"
#include "xla/core/collectives/communicator.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/service/collective_ops_utils.h"
#include "xla/service/gpu/runtime/nccl_collective_thunk.h"
#include "xla/service/gpu/runtime/thunk.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/memory_allocation.h"
#include "xla/stream_executor/stream.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {
namespace {

NcclRaggedAllToAllConfig GetNcclRaggedAllToAllConfig(
    const HloRaggedAllToAllInstruction* instr) {
  NcclRaggedAllToAllConfig config;
  config.config = GetNcclCollectiveConfig(instr, std::nullopt);
  config.num_ragged_rows = instr->operand(2)->shape().dimensions(0);
  config.ragged_row_element_size =
      ShapeUtil::ElementsIn(instr->shape()) / instr->shape().dimensions(0);
  return config;
}

// A wrapper around an raw data buffer that indexes values based on the
// PrimitiveType that is stored in the buffer.
class IntegerOperandData {
 public:
  IntegerOperandData(PrimitiveType element_type, void* data)
      : element_type_(element_type), data_(data) {}

  int64_t get(int i) const {
    switch (element_type_) {
      case PrimitiveType::S32:
      case PrimitiveType::U32:
        return reinterpret_cast<int32_t*>(data_)[i];
      case PrimitiveType::S64:
      case PrimitiveType::U64:
        return reinterpret_cast<int64_t*>(data_)[i];
      default:
        LOG(FATAL) << "Unsupported element type: " << element_type_;
    }
  }

  int64_t operator[](int i) const { return get(i); }

 private:
  PrimitiveType element_type_;
  void* data_;
};

// Loads the offsets and sizes from device memory into the host buffer.
absl::StatusOr<std::vector<IntegerOperandData>> LoadOffsetAndSizeOperands(
    se::Stream& stream, std::vector<DeviceBufferPair>& buffers,
    int64_t* host_buffer_alloc) {
  int64_t num_elements = buffers[2].element_count;

  std::vector<IntegerOperandData> indices;
  for (int i = 2; i < buffers.size(); ++i) {
    TF_RETURN_IF_ERROR(stream.Memcpy(host_buffer_alloc,
                                     buffers[i].source_buffer,
                                     buffers[i].source_buffer.size()));
    indices.push_back(
        IntegerOperandData(buffers[i].element_type, host_buffer_alloc));
    host_buffer_alloc += num_elements;
  }

  // Wait for the copies to complete.
  if (absl::Status blocked = stream.BlockHostUntilDone(); !blocked.ok()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to complete all kernels launched on stream %p: %s", &stream,
        blocked.message()));
  }

  return indices;
}

}  // namespace

NcclRaggedAllToAllStartThunk::NcclRaggedAllToAllStartThunk(
    ThunkInfo thunk_info, const HloRaggedAllToAllInstruction* instr,
    std::vector<NcclCollectiveThunk::Buffer> buffers, bool p2p_memcpy_enabled)
    : NcclCollectiveThunk(Thunk::kNcclAllToAllStart, thunk_info,
                          IsSyncCollective(instr)),
      config_(GetNcclRaggedAllToAllConfig(instr)),
      buffers_(std::move(buffers)) {
  CHECK_EQ(config_.config.operand_count, buffers_.size());
}

/*static*/ absl::Status NcclRaggedAllToAllStartThunk::CheckImplementable(
    const HloRaggedAllToAllInstruction* instr, int64_t replica_count,
    int64_t partition_count) {
  auto status = [&instr]() -> absl::Status {
    for (HloInstruction* operand : instr->operands()) {
      Shape shape = operand->shape();
      TF_RETURN_IF_ERROR(IsValidOperand(shape, Thunk::kNcclRaggedAllToAll));
    }
    return absl::OkStatus();
  };
  return AddOpDescription<NcclRaggedAllToAllStartThunk>(
      status(), instr, replica_count, partition_count);
}

/*static*/ CollectiveOpGroupMode NcclRaggedAllToAllStartThunk::GetGroupMode(
    const HloRaggedAllToAllInstruction* instr) {
  return GetNcclRaggedAllToAllConfig(instr).config.group_mode;
}

absl::Status NcclRaggedAllToAllStartThunk::Initialize(
    const InitializeParams& params) {
  TF_RETURN_IF_ERROR(NcclCollectiveThunk::Initialize(params));

  // Allocate a temp host buffer to load the sizes and offsets from device
  // memory. We use one buffer to read all the sizes and offsets, so we need 4
  // times the number of ragged rows. Sizes and offsets can be int32 or int64,
  // so we allocate 8 bytes per element.
  absl::MutexLock lock(&mutex_);
  if (auto it = host_buffer_allocs_.find(params.executor);
      it == host_buffer_allocs_.end()) {
    TF_ASSIGN_OR_RETURN(std::unique_ptr<se::MemoryAllocation> allocation,
                        params.executor->HostMemoryAllocate(
                            4 * config_.num_ragged_rows * sizeof(int64_t)));
    host_buffer_allocs_.emplace(params.executor, std::move(allocation));
  }

  return absl::OkStatus();
}

absl::Status NcclRaggedAllToAllStartThunk::RunNcclCollective(
    const ExecuteParams& params, se::Stream& stream,
    CommunicatorHandle comm_handle) {
  TF_ASSIGN_OR_RETURN(
      std::vector<DeviceBufferPair> device_buffers,
      ConvertToDeviceBuffers(params, buffers_,
                             config_.config.operand_element_type));

  TF_ASSIGN_OR_RETURN(GpuCollectives * collectives, GetGpuCollectives(params));

  // Get the temp buffer alloc to load sizes and offsets from device memory.
  int64_t* host_buffer_alloc = [&] {
    absl::MutexLock lock(&mutex_);
    return reinterpret_cast<int64_t*>(
        host_buffer_allocs_.at(stream.parent())->opaque());
  }();

  return xla::gpu::RunRaggedAllToAll(
      collectives, config_.ragged_row_element_size, device_buffers, stream,
      comm_handle.comm, host_buffer_alloc);
}

AsyncStreamKind NcclRaggedAllToAllStartThunk::GetAsyncStreamKind() const {
  return AsyncStreamKind::kCollective;
}

absl::Status RunRaggedAllToAll(GpuCollectives* collectives,
                               int64_t ragged_row_element_size,
                               std::vector<DeviceBufferPair>& buffers,
                               se::Stream& stream, Communicator* comm,
                               int64_t* host_buffer_alloc) {
  int device_ordinal = stream.parent()->device_ordinal();
  VLOG(3) << "Performing ragged-all-to-all from device ordinal: "
          << device_ordinal;
  TF_RETURN_IF_ERROR(
      MaybeRegisterBuffers(collectives, stream.parent(), buffers, comm));

  TF_ASSIGN_OR_RETURN(int32_t num_ranks, comm->NumRanks());

  TF_ASSIGN_OR_RETURN(
      std::vector<IntegerOperandData> index_operands,
      LoadOffsetAndSizeOperands(stream, buffers, host_buffer_alloc));

  const IntegerOperandData& input_offsets = index_operands[0];
  const IntegerOperandData& send_sizes = index_operands[1];
  const IntegerOperandData& output_offsets = index_operands[2];
  const IntegerOperandData& recv_sizes = index_operands[3];

  TF_RETURN_IF_ERROR(collectives->GroupStart());

  DeviceBufferPair& data_buffer = buffers[0];
  for (int peer = 0; peer < num_ranks; ++peer) {
    se::DeviceMemoryBase send_slice =
        collectives->Slice(data_buffer.source_buffer, data_buffer.element_type,
                           input_offsets[peer] * ragged_row_element_size,
                           send_sizes[peer] * ragged_row_element_size);

    se::DeviceMemoryBase recv_slice = collectives->Slice(
        data_buffer.destination_buffer, data_buffer.element_type,
        output_offsets[peer] * ragged_row_element_size,
        recv_sizes[peer] * ragged_row_element_size);

    TF_RETURN_IF_ERROR(comm->Send(send_slice, data_buffer.element_type,
                                  send_sizes[peer] * ragged_row_element_size,
                                  peer, GpuCollectives::On(stream)));

    TF_RETURN_IF_ERROR(comm->Recv(recv_slice, data_buffer.element_type,
                                  recv_sizes[peer] * ragged_row_element_size,
                                  peer, GpuCollectives::On(stream)));
  }

  return collectives->GroupEnd();
}

}  // namespace gpu
}  // namespace xla
