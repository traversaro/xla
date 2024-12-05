/* Copyright 2020 Google LLC

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

#include "xla/pjrt/distributed/client.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "grpcpp/channel.h"
#include "xla/pjrt/distributed/key_value_store_interface.h"
#include "xla/tsl/distributed_runtime/coordination/coordination_client.h"
#include "xla/tsl/distributed_runtime/coordination/coordination_service_agent.h"
#include "xla/tsl/distributed_runtime/rpc/coordination/grpc_coordination_client.h"
#include "xla/tsl/protobuf/coordination_config.pb.h"
#include "xla/tsl/protobuf/coordination_service.pb.h"
#include "tsl/platform/statusor.h"

namespace xla {

class DistributedRuntimeCoordinationServiceClient
    : public DistributedRuntimeClient {
 public:
  DistributedRuntimeCoordinationServiceClient(
      std::shared_ptr<::grpc::Channel> channel, const Options& options);
  explicit DistributedRuntimeCoordinationServiceClient(
      std::shared_ptr<::grpc::Channel> channel)
      : DistributedRuntimeCoordinationServiceClient(channel, Options()) {}
  ~DistributedRuntimeCoordinationServiceClient() override;

  absl::Status Connect() override;
  absl::Status Shutdown() override;
  absl::StatusOr<std::string> BlockingKeyValueGet(
      std::string_view key, absl::Duration timeout) override;
  absl::StatusOr<std::vector<std::pair<std::string, std::string>>>
  KeyValueDirGet(std::string_view key) override;
  absl::Status KeyValueSet(std::string_view key,
                           std::string_view value) override;
  absl::Status KeyValueSet(std::string_view key, std::string_view value,
                           bool allow_overwrite) override;
  absl::Status KeyValueDelete(std::string_view key) override;
  absl::Status WaitAtBarrier(
      std::string barrier_id, absl::Duration timeout,
      std::optional<absl::Span<const int32_t>> process_ids) override;
  absl::StatusOr<std::vector<int32_t>> GetAliveNodes(
      absl::Span<const int32_t> nodes) override;
  absl::StatusOr<tsl::CoordinationServiceAgent*> GetCoordinationServiceAgent()
      override;

 private:
  std::unique_ptr<tsl::CoordinationServiceAgent> coord_agent_;
  tensorflow::CoordinationServiceConfig config_;
  absl::Duration min_connect_barrier_timeout_;
  int task_id_;
};

DistributedRuntimeCoordinationServiceClient::
    DistributedRuntimeCoordinationServiceClient(
        std::shared_ptr<::grpc::Channel> channel, const Options& options) {
  // Convert options to coordination config.
  tensorflow::CoordinationServiceConfig config;
  config.set_service_type("standalone");
  config.set_service_leader("/job:jax_worker/task:0");
  config.set_cluster_register_timeout_in_ms(
      absl::ToInt64Milliseconds(options.init_timeout));
  config.set_heartbeat_timeout_in_ms(absl::ToInt64Milliseconds(
      options.heartbeat_interval * options.max_missing_heartbeats));
  config.set_cluster_register_with_barrier(true);
  config.set_shutdown_barrier_timeout_in_ms(
      absl::ToInt64Milliseconds(options.shutdown_timeout));
  config.set_agent_destruction_without_shutdown(
      !options.shutdown_on_destruction);
  config.set_poll_for_error_from_service_at_startup(
      options.poll_for_error_from_service_at_startup);
  auto error_fn = [timeout_fn = options.missed_heartbeat_callback](
                      const absl::Status& status) { timeout_fn(status); };

  std::unique_ptr<tsl::CoordinationClient> leader_client;
  leader_client.reset(tsl::NewGrpcCoordinationClient(channel));
  coord_agent_ = tsl::CreateCoordinationServiceAgent();
  const absl::Status status =
      coord_agent_->Initialize(options.env, "jax_worker", options.node_id,
                               config, std::move(leader_client), error_fn);
  if (!status.ok()) {
    LOG(ERROR) << "Coordination agent failed to initialize: " << status;
  }
  task_id_ = options.node_id;
  config_ = config;
}

DistributedRuntimeCoordinationServiceClient::
    ~DistributedRuntimeCoordinationServiceClient() = default;

absl::Status DistributedRuntimeCoordinationServiceClient::Connect() {
  absl::Status s = coord_agent_->Connect();

  if (s.ok()) {
    LOG(INFO) << "Connected to distributed JAX controller";
  } else if (absl::IsDeadlineExceeded(s)) {
    LOG(ERROR)
        << "Failed to connect to distributed JAX controller: waited too "
           "long for some tasks to show up. This may be due to 1) some "
           "tasks crashed earlier before connecting, 2) some tasks were never "
           "scheduled, or 3) scheduling delays. Consider setting a longer "
           "initialization timeout if such delays are expected, the timeout is "
           "currently set to: "
        << absl::Milliseconds(config_.cluster_register_timeout_in_ms())
        << ".\n\nOriginal runtime error: " << s;
  } else {
    LOG(ERROR) << "Failed to connect to distributed JAX controller: " << s;
  }
  return s;
}

absl::Status DistributedRuntimeCoordinationServiceClient::Shutdown() {
  LOG(INFO) << "Distributed task shutdown initiated.";
  absl::Status s = coord_agent_->Shutdown();
  LOG(INFO) << "Distributed task shutdown result: " << s;
  return s;
}

absl::StatusOr<std::string>
DistributedRuntimeCoordinationServiceClient::BlockingKeyValueGet(
    std::string_view key, absl::Duration timeout) {
  return coord_agent_->GetKeyValue(key, timeout);
}

absl::StatusOr<std::vector<std::pair<std::string, std::string>>>
DistributedRuntimeCoordinationServiceClient::KeyValueDirGet(
    std::string_view key) {
  TF_ASSIGN_OR_RETURN(const auto results, coord_agent_->GetKeyValueDir(key));

  std::vector<std::pair<std::string, std::string>> kvs;
  kvs.reserve(results.size());

  // Convert tensorflow::KeyValueEntry to std::pair<std::string,
  // string>.
  for (const auto& kv : results) {
    kvs.push_back(std::make_pair(kv.key(), kv.value()));
  }
  return kvs;
}

absl::Status DistributedRuntimeCoordinationServiceClient::KeyValueDelete(
    std::string_view key) {
  return coord_agent_->DeleteKeyValue(key);
}

absl::Status DistributedRuntimeCoordinationServiceClient::KeyValueSet(
    std::string_view key, std::string_view value) {
  return KeyValueSet(key, value, /*allow_overwrite=*/false);
}

absl::Status DistributedRuntimeCoordinationServiceClient::KeyValueSet(
    std::string_view key, std::string_view value, bool allow_overwrite) {
  return coord_agent_->InsertKeyValue(key, value, allow_overwrite);
}

absl::Status DistributedRuntimeCoordinationServiceClient::WaitAtBarrier(
    std::string barrier_id, absl::Duration timeout,
    std::optional<absl::Span<const int32_t>> process_ids) {
  std::vector<tensorflow::CoordinatedTask> tasks;
  if (process_ids.has_value()) {
    tasks.reserve(process_ids->size());
    for (int32_t process_id : process_ids.value()) {
      tensorflow::CoordinatedTask task;
      task.set_job_name("jax_worker");
      task.set_task_id(process_id);
      tasks.push_back(std::move(task));
    }
  }
  return coord_agent_->WaitAtBarrier(barrier_id, timeout, tasks);
}

absl::StatusOr<std::vector<int32_t>>
DistributedRuntimeCoordinationServiceClient::GetAliveNodes(
    absl::Span<const int32_t> nodes) {
  // Note that jax.distributed uses terms "process" and "node", and the
  // coordination service uses the term "task". These all refer to the same
  // thing, and it's why you see us use both sets of terms as we cross the
  // abstraction boundary from jax.distributed into the coordination service.

  // Wrap the the node ids into tasks.
  std::vector<tensorflow::CoordinatedTask> tasks;
  for (int32_t task_id : nodes) {
    tensorflow::CoordinatedTask task;
    task.set_job_name("jax_worker");
    task.set_task_id(task_id);
    tasks.push_back(std::move(task));
  }

  // Get the set of alive tasks.
  TF_ASSIGN_OR_RETURN(
      const std::vector<tensorflow::CoordinatedTask> alive_tasks,
      coord_agent_->GetAliveTasks(tasks));

  // Extract the node ids from the alive tasks.
  std::vector<int32_t> alive_nodes(alive_tasks.size());
  for (int i = 0; i < alive_tasks.size(); ++i) {
    alive_nodes[i] = alive_tasks[i].task_id();
  }
  return alive_nodes;
}

absl::StatusOr<tsl::CoordinationServiceAgent*>
DistributedRuntimeCoordinationServiceClient::GetCoordinationServiceAgent() {
  return coord_agent_.get();
}

std::unique_ptr<DistributedRuntimeClient> GetDistributedRuntimeClient(
    std::shared_ptr<::grpc::Channel> channel,
    const DistributedRuntimeClient::Options& options) {
  return std::make_unique<xla::DistributedRuntimeCoordinationServiceClient>(
      channel, options);
}

namespace {

class DistributedKeyValueStore : public KeyValueStoreInterface {
 public:
  DistributedKeyValueStore(std::shared_ptr<DistributedRuntimeClient> client,
                           std::string prefix)
      : client_(std::move(client)), prefix_(std::move(prefix)) {}

  absl::StatusOr<std::string> Get(std::string_view key,
                                  absl::Duration timeout) override {
    return client_->BlockingKeyValueGet(absl::StrCat(prefix_, key), timeout);
  }

  absl::Status Set(std::string_view key, std::string_view value) override {
    return client_->KeyValueSet(absl::StrCat(prefix_, key), value);
  }

 private:
  std::shared_ptr<DistributedRuntimeClient> client_;
  std::string prefix_;
};

}  // namespace

std::shared_ptr<KeyValueStoreInterface> GetDistributedKeyValueStore(
    std::shared_ptr<DistributedRuntimeClient> client, std::string prefix) {
  return std::make_shared<DistributedKeyValueStore>(std::move(client),
                                                    std::move(prefix));
}

}  // namespace xla
