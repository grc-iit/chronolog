#pragma once
#include "tier/FileTierStore.h"
#include <atomic>

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
    uint32_t probe_interval_ms = 1000;
    uint32_t probe_timeout_ms = 1000;
    uint32_t io_timeout_ms = 1000;
    uint32_t io_threads = 2;
    std::string status_file;
    std::string writer;
    std::vector<GrapherTier> tiers;
};
class MigrationWorker
{
public:
    MigrationWorker(FileTierStore& store, MigrationSettings settings);
    absl::Status pass();
    void stop() { stopped_ = true; }

private:
    FileTierStore& store_;
    const MigrationSettings settings_;
    std::atomic<bool> stopped_{};
    double tokens_{};
    std::chrono::steady_clock::time_point refilled_;
    std::vector<bool> draining_;
};
} // namespace chronolog::grapher
