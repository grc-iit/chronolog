#include "chrono-grapher/server/MigrationWorker.h"
#include "worker/WorkerPool.h"
#include "tier/FileIO.h"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace chronolog::grapher
{
namespace
{
double Usage(const GrapherTier& tier, const FileTierStore::TierUsage& usage)
{
    double fraction = tier.budget_bytes ? static_cast<double>(usage.used_bytes) / tier.budget_bytes : 0;
    if(usage.total_bytes)
        fraction = std::max(fraction,
                            static_cast<double>(usage.total_bytes - usage.free_bytes) /
                                    (usage.total_bytes * (1 - tier.min_free_fraction)));
    return fraction;
}
} // namespace
MigrationWorker::MigrationWorker(FileTierStore& store, MigrationSettings settings, bool scrub_enabled)
    : store_(store)
    , settings_(std::move(settings))
    , tokens_(settings_.io_bytes_per_sec)
    , refilled_(std::chrono::steady_clock::now())
    , draining_(settings_.tiers.size())
    , available_(settings_.tiers.size())
{
    scrub_.enabled = scrub_enabled;
}

void MigrationWorker::scrubbed(const absl::StatusOr<ScrubResult>& result, int64_t finished_at_unix_ms)
{
    std::lock_guard lock(scrub_mutex_);
    if(!result.ok())
    {
        scrub_.error = result.status().ToString();
        return;
    }
    scrub_.last = *result;
    scrub_.finished_at_unix_ms = finished_at_unix_ms;
    scrub_.error.clear();
}

absl::Status MigrationWorker::pass()
{
    CHRONOLOG_ASSERT_WORKER_THREAD();
    if(stopped_)
        return absl::CancelledError("migration worker stopped");
    (void)store_.probeTiers(std::chrono::milliseconds(settings_.probe_timeout_ms));
    const auto now = std::chrono::steady_clock::now();
    tokens_ = std::min<double>(256 * 1024 * 1024,
                               tokens_ + std::chrono::duration<double>(now - refilled_).count() *
                                                 settings_.io_bytes_per_sec);
    refilled_ = now;
    const auto wall =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    const auto cutoff = wall - static_cast<int64_t>(settings_.after_s) * 1000000000;
    if(settings_.enabled && !store_.migrationStopped())
        for(size_t source = 0; source + 1 < settings_.tiers.size() && !stopped_; ++source)
        {
            auto usage = store_.tierUsage(settings_.tiers[source].config.name);
            if(!usage.ok() || !usage->available)
                continue;
            const auto fraction = Usage(settings_.tiers[source], *usage);
            if(fraction > settings_.tiers[source].high_watermark)
                draining_[source] = true;
            if(fraction < settings_.tiers[source].low_watermark)
                draining_[source] = false;
            for(size_t job = 0; job < 128 && !stopped_ && !store_.migrationStopped(); ++job)
            {
                size_t destination = source + 1;
                for(; destination < settings_.tiers.size(); ++destination)
                {
                    auto target = store_.tierUsage(settings_.tiers[destination].config.name);
                    if(target.ok() && target->available &&
                       Usage(settings_.tiers[destination], *target) < settings_.tiers[destination].high_watermark)
                        break;
                }
                if(destination == settings_.tiers.size())
                    break;
                uint64_t copied = 0;
                auto moved = store_.migrateOnce(settings_.tiers[destination].config.name,
                                                settings_.tiers[source].config.rank,
                                                draining_[source] ? INT64_MAX : cutoff,
                                                static_cast<uint64_t>(tokens_),
                                                &copied);
                tokens_ = std::max(0.0, tokens_ - static_cast<double>(copied));
                if(!moved.ok())
                {
                    // An attempt that stopped part way can leave a temporary, a destination or a stale source.
                    cleanup_due_ = sweep_due_ = true;
                    break;
                }
                if(!*moved)
                    break;
                replicas_due_ = true;
                usage = store_.tierUsage(settings_.tiers[source].config.name);
                if(!usage.ok() || !usage->available)
                    break;
                if(Usage(settings_.tiers[source], *usage) < settings_.tiers[source].low_watermark)
                    draining_[source] = false;
            }
        }
    if(stopped_)
        return absl::CancelledError("migration worker stopped");
    std::vector<FileTierStore::TierUsage> usages;
    for(size_t i = 0; i < settings_.tiers.size(); ++i)
    {
        auto usage = store_.tierUsage(settings_.tiers[i].config.name);
        usages.push_back(usage.ok() ? *usage : FileTierStore::TierUsage{});
        if(usages.back().available && !available_[i])
            cleanup_due_ = sweep_due_ = replicas_due_ = true;
        available_[i] = usages.back().available;
    }
    if(cleanup_due_ && !store_.migrationStopped() && store_.cleanupMigrations().ok())
        cleanup_due_ = false;
    if(sweep_due_ && store_.sweepTiers().ok())
        sweep_due_ = false;
    if(replicas_due_ && !store_.migrationStopped() &&
       std::all_of(available_.begin(), available_.end(), [](bool available) { return available; }) &&
       store_.writeTierReplicas().ok())
        replicas_due_ = false;
    if(settings_.status_file.empty())
        return absl::OkStatus();
    ScrubStatus scrub;
    {
        std::lock_guard lock(scrub_mutex_);
        scrub = scrub_;
    }
    nlohmann::json status{{"writer", settings_.writer},
                          {"migrate_enabled", settings_.enabled},
                          {"migration_stopped", store_.migrationStopped()},
                          {"heartbeat_ms", settings_.status_heartbeat_ms},
                          {"scrub",
                           {{"enabled", scrub.enabled},
                            {"validated", scrub.last.validated},
                            {"skipped", scrub.last.skipped},
                            {"lost", scrub.last.lost},
                            {"rolled_back", scrub.last.rolled_back},
                            {"slow_failed", scrub.last.slow_failed},
                            {"through", scrub.last.through},
                            {"finished_at_unix_ms", scrub.finished_at_unix_ms},
                            {"error", scrub.error}}},
                          {"tiers", nlohmann::json::array()},
                          {"pending_tier_deletions", nlohmann::json::object()}};
    for(size_t i = 0; i < settings_.tiers.size(); ++i)
    {
        const auto& tier = settings_.tiers[i];
        const auto& value = usages[i];
        status["tiers"].push_back({{"name", tier.config.name},
                                   {"rank", tier.config.rank},
                                   {"available", value.available},
                                   {"used_bytes", value.used_bytes},
                                   {"budget_bytes", tier.budget_bytes},
                                   {"above_high", value.available && Usage(tier, value) > tier.high_watermark}});
    }
    for(const auto& [story, count]: store_.pendingTierDeletions())
        status["pending_tier_deletions"][std::to_string(story)] = count;
    // Rewritten when it says something new, and on the heartbeat so a reader can tell this worker is alive. validate()
    // keeps the path off every slow tier root.
    auto content = status.dump();
    const auto steady = settings_.steady_now ? settings_.steady_now() : std::chrono::steady_clock::now();
    if(content == written_status_ && steady - written_at_ < std::chrono::milliseconds(settings_.status_heartbeat_ms))
        return absl::OkStatus();
    status["written_at_unix_ms"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    const auto text = status.dump() + "\n";
    const auto temporary = settings_.status_file + ".tmp";
    tier_detail::Fd fd(::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644));
    if(fd.get() < 0)
        return tier_detail::IoError("open tier status temporary");
    auto result = tier_detail::WriteAll(fd.get(), text);
    if(result.ok() && ::fsync(fd.get()) != 0)
        result = tier_detail::IoError("sync tier status");
    if(result.ok() && ::rename(temporary.c_str(), settings_.status_file.c_str()) != 0)
        result = tier_detail::IoError("rename tier status");
    if(result.ok())
    {
        written_status_ = std::move(content);
        written_at_ = steady;
    }
    return result;
}
} // namespace chronolog::grapher
