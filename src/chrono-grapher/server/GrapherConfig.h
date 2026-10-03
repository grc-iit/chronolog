#pragma once

#include "chrono-grapher/server/ArchiveService.h"
#include <optional>

namespace chronolog::grapher
{
struct GrapherConfig
{
    std::string process_id = "grapher-1";
    std::string manifest_writer;
    std::string internal_listen = "127.0.0.1:50063";
    std::string self_endpoint = "127.0.0.1:50063";
    std::string visor_internal = "127.0.0.1:50061";
    std::string archive_root = "./archive";
    std::string archive_codec = "hdf5";
    // Stderr threshold: info, warning or error.
    std::string log_level = "info";
    uint32_t heartbeat_interval_ms = 1000;
    uint32_t rpc_timeout_ms = 2000;
    uint32_t drain_timeout_ms = 5000;
    TransferLimits limits;
    CompactionSettings compaction;
    bool insecure_bind_all = false;

    static absl::StatusOr<GrapherConfig> load(const std::optional<std::string>& path, bool allow_bind_all = false);
    absl::Status validate() const;
};
} // namespace chronolog::grapher
