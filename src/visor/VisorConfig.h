#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "chronolog/types.h"
#include "catalog/LeaseAuthority.h"

namespace chronolog::visor
{

// Visor configuration. Keys come from a JSON file and are overridden by environment
// variables named CHRONOLOG_VISOR_<KEY>, for example CHRONOLOG_VISOR_DB_PATH. The
// keepers key is a comma separated list of process_id=endpoint pairs in the environment.
struct RaftPeer
{
    int32_t id{};
    std::string raft_endpoint;
    std::string catalog_endpoint;
    std::string internal_endpoint;
};
struct RaftConfig
{
    int32_t server_id{};
    std::string raft_endpoint;
    std::vector<RaftPeer> peers;
    uint32_t election_lower_ms = 300;
    uint32_t election_upper_ms = 600;
    uint32_t heartbeat_ms = 75;
};
struct VisorConfig
{
    std::string membership_mode = "static";
    RaftConfig raft;
    AcquisitionLeaseConfig leases;
    std::string listen = "0.0.0.0:50051";
    // Cluster service address. Loopback by default so a bare start never exposes it (S14.3).
    std::string internal_listen = "127.0.0.1:50061";
    // Relative to the working directory, so a local run needs no privileges.
    std::string db_path = "./catalog.sqlite";
    std::vector<KeeperRef> keepers = {{"keeper-1", "chrono-keeper:50052"}};
    std::string grapher = "chrono-grapher:50053";
    std::vector<std::string> graphers;
    std::string player = "chrono-player:50054";
    // Recorded in section 13 of ARCHITECTURE.md (heartbeat timeout row): 15000.
    uint32_t heartbeat_timeout_ms = 15000;
    // How long Release waits for the assigned Keeper to apply the fence.
    uint32_t release_fence_timeout_ms = 2000;
    uint32_t worker_threads = 4;
    // Allows internal_listen on a wildcard address. Refused otherwise (S14.3).
    bool insecure_bind_all = false;
    // Stderr threshold: info, warning or error.
    std::string log_level = "info";

    using Getenv = std::function<const char*(const char*)>;

    // Defaults, then the JSON file when `path` is given, then the environment.
    static absl::StatusOr<VisorConfig> load(
            const std::optional<std::string>& path,
            const Getenv& getenv = [](const char* name) { return std::getenv(name); },
            bool allow_bind_all = false);

    absl::Status validate() const;
};

} // namespace chronolog::visor
