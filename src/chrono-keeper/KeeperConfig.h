#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "chronolog/types.h"

namespace chronolog::keeper
{

struct StaticRoute
{
    StoryId story_id{};
    Route route;
};

// A writer admitted at boot, for a Keeper running without a Visor.
struct StaticWriter
{
    StoryId story_id{};
    uint64_t writer_id{};
    uint64_t incarnation{};
};

// Keeper configuration. Keys come from a JSON file and are overridden by environment
// variables named CHRONOLOG_KEEPER_<KEY>, for example CHRONOLOG_KEEPER_PROCESS_ID.
// static_routes and static_writers are file only.
struct KeeperConfig
{
    std::string listen = "0.0.0.0:50052";
    // Archive service address. Loopback by default so a bare start never exposes it (S14.3).
    std::string internal_listen = "127.0.0.1:50062";
    std::string process_id = "keeper-1";
    // Stderr threshold: info, warning or error.
    std::string log_level = "info";
    // Must equal the endpoint the Visor lists for this Keeper.
    std::string self_endpoint = "chrono-keeper:50052";
    std::string visor_internal = "chrono-visor:50061";
    size_t payload_max_bytes = 1048576;
    // TBD in section 13 of ARCHITECTURE.md; this PR proposes 60 s.
    int64_t causal_floor_skew_limit_ns = 60'000'000'000;
    size_t dedupe_window = 65536;
    std::string wal_dir = "wal";
    size_t group_commit_max_bytes = 4u << 20;
    // I5.10 group-commit window in microseconds, at most 10000. Zero commits each group as soon as the
    // WAL committer is free; nonzero holds a group open that long after its first record.
    uint32_t group_commit_window_us = 0;
    uint32_t reserve_ahead_ms = 1000;
    uint64_t wal_max_bytes = 1ull << 30;
    uint64_t wal_segment_bytes = 64ull << 20;
    uint32_t shutdown_confirm_timeout_secs = 150;
    uint32_t story_chunk_duration_secs = 10;
    uint32_t seal_interval_ms = 1000;
    size_t chunk_max_bytes = 32u << 20;
    uint32_t chunk_max_events = 65536;
    size_t frame_bytes = 1u << 20;
    uint32_t watermark_resend_timeout_secs = 300;
    uint32_t archive_visibility_delay_secs = 10;
    uint64_t retention_cap_mb = 4096;
    // Zero selects std::thread::hardware_concurrency().
    uint32_t worker_threads = 0;
    uint32_t heartbeat_interval_ms = 5000;
    // Must equal the Visor's keeper_failure_timeout_ms and release_fence_timeout_ms: the deadline of every
    // call that feeds those timers is derived from them (M11.3).
    uint32_t keeper_failure_timeout_ms = 15000;
    uint32_t release_fence_timeout_ms = 2000;
    uint32_t append_ceiling_wait_ms = 1000;
    // Allows internal_listen on a wildcard address. Refused otherwise (S14.3).
    bool insecure_bind_all = false;
    std::vector<StaticRoute> static_routes;
    std::vector<StaticWriter> static_writers;

    using Getenv = std::function<const char*(const char*)>;

    // Defaults, then the JSON file when `path` is given, then the environment.
    static absl::StatusOr<KeeperConfig> load(
            const std::optional<std::string>& path,
            const Getenv& getenv = [](const char* name) { return std::getenv(name); },
            bool allow_bind_all = false);

    absl::Status validate() const;
    uint32_t effectiveWorkerThreads() const;
    // Register and Heartbeat must answer before the next heartbeat can be missed and before a release fence
    // times out.
    std::chrono::milliseconds heartbeatDeadline() const;
};

} // namespace chronolog::keeper
