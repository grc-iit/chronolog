#pragma once

#include <chrono>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

#include <absl/status/statusor.h>

#include "chrono-grapher/server/VisorClockAudit.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "chronolog/types.h"

namespace chronolog::grapher
{

struct ClusterWorkerOptions
{
    std::string process_id;
    std::string instance;
    std::string endpoint;
    std::chrono::milliseconds rpc_timeout{2000};
    std::chrono::milliseconds heartbeat_interval{1000};
    std::chrono::steady_clock::time_point process_start{std::chrono::steady_clock::now()};
    // Stories whose archive predates the physical policy, reported in bounded heartbeat pages.
    std::function<absl::StatusOr<std::vector<StoryId>>()> stories_without_physical_policy;
};

// Registers with the Visor, then heartbeats until stopped, auditing the clock on every attempt.
void runClusterWorker(std::stop_token stop,
                      internal::v1::Cluster::StubInterface& stub,
                      VisorClockAudit& audit,
                      const ClusterWorkerOptions& options);

} // namespace chronolog::grapher
