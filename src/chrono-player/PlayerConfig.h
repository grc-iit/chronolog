#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include "chronolog/types.h"
#include "tier/PosixTier.h"

namespace chronolog::player
{

// Player configuration. Keys come from a JSON file and are overridden by environment variables
// named CHRONOLOG_PLAYER_<KEY>. keeper_internal is a comma separated list of process_id=address
// pairs in the environment. static_routes is JSON only.
struct PlayerConfig
{
    std::string listen = "0.0.0.0:50054";
    // Endpoint the Visor hands to clients as Route.player; defaults to listen when empty.
    std::string advertise;
    std::string player_id = "player-1";
    // Catalog address. Empty disables the tombstone check, for runs without a Visor.
    std::string visor = "chrono-visor:50051";
    // Cluster address. Unused when static_routes is set.
    std::string visor_internal = "chrono-visor:50061";
    // Keeper internal listener is the Route endpoint host plus this suffix unless keeper_internal names it.
    std::string keeper_internal_suffix = ":50062";
    std::map<std::string, std::string> keeper_internal;
    uint32_t keeper_deadline_ms = 2000;
    uint32_t batch_size = 1024;
    uint32_t read_max_events = 262144;
    uint32_t tail_max_bytes = 64 * 1024 * 1024;
    uint32_t tail_poll_ms = 200;
    std::string archive_root;
    uint32_t manifest_poll_ms = 1000;
    // 30 seconds leaves ample headroom for healthy NVMe and NFS loads; the largest
    // deployed NFS chunk (197632 bytes) took 10 ms with the client cache bypassed.
    uint32_t archive_read_timeout_ms = 30000;
    // Stderr threshold: info, warning or error.
    std::string log_level = "info";
    // One Route for every story, replacing registration with the Visor.
    std::optional<Route> static_routes;

    std::string deployment_id;
    std::vector<TierConfig> tiers;
    uint32_t tier_io_timeout_ms = 1000;
    uint32_t tier_probe_interval_ms = 10000;
    uint32_t tier_probe_timeout_ms = 1000;
    uint32_t slow_tier_io_threads = 2;

    using Getenv = std::function<const char*(const char*)>;

    static absl::StatusOr<PlayerConfig> load(
            const std::optional<std::string>& path,
            const Getenv& getenv = [](const char* name) { return std::getenv(name); });

    absl::Status validate() const;

    std::string keeperInternal(const KeeperRef& keeper) const;
};

} // namespace chronolog::player
