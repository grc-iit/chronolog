#pragma once
#include "tier/FileTierStore.h"
#include <atomic>
#include <functional>
#include <mutex>

namespace chronolog::grapher
{
struct GrapherTier
{
    TierConfig config;
    uint64_t budget_bytes = 0;
    double min_free_fraction = 0.05;
    double high_watermark = 0.9;
    double low_watermark = 0.75;
};
struct MigrationSettings
{
    bool enabled = false;
    uint64_t after_s = 3600;
    uint64_t io_bytes_per_sec = 4 * 1024 * 1024;
    uint32_t probe_interval_ms = 5000;
    uint32_t probe_timeout_ms = 1000;
    uint32_t io_timeout_ms = 1000;
    uint32_t io_threads = 2;
    std::string status_file;
    // The status file is rewritten at least this often by a live tier worker, so a reader can tell a stale one.
    uint32_t status_heartbeat_ms = 10000;
    // Replaces steady_clock::now for the heartbeat, for tests.
    std::function<std::chrono::steady_clock::time_point()> steady_now;
    std::string writer;
    std::vector<GrapherTier> tiers;
};
// The last scrub pass as the status file reports it; the scrub loop hands it over, the writer does no store I/O.
struct ScrubStatus
{
    bool enabled{};
    ScrubResult last;
    int64_t finished_at_unix_ms{};
    std::string error;
};
class MigrationWorker
{
public:
    MigrationWorker(FileTierStore& store, MigrationSettings settings, bool scrub_enabled = false);
    absl::Status pass();
    void stop() { stopped_ = true; }
    // A finished pass replaces the counts and clears the error; a failed one sets the error and keeps the counts.
    void scrubbed(const absl::StatusOr<ScrubResult>& result, int64_t finished_at_unix_ms);

private:
    FileTierStore& store_;
    const MigrationSettings settings_;
    std::atomic<bool> stopped_{};
    double tokens_{};
    std::chrono::steady_clock::time_point refilled_;
    std::vector<bool> draining_;
    // Work that follows an event, not the clock: stale copies and leftovers exist after a start, a failed attempt or
    // a tier's return, and a replica is stale after a migration.
    std::vector<bool> available_;
    bool cleanup_due_ = true, sweep_due_ = true, replicas_due_ = true;
    std::string written_status_;
    std::chrono::steady_clock::time_point written_at_;
    std::mutex scrub_mutex_;
    ScrubStatus scrub_;
};
} // namespace chronolog::grapher
