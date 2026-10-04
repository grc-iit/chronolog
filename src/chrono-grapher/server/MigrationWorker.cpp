#include "chrono-grapher/server/MigrationWorker.h"
#include "chrono-grapher/server/WorkerPool.h"
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
MigrationWorker::MigrationWorker(FileTierStore& store, MigrationSettings settings)
    : store_(store)
    , settings_(std::move(settings))
    , tokens_(settings_.io_bytes_per_sec)
    , refilled_(std::chrono::steady_clock::now())
    , draining_(settings_.tiers.size())
{}

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
                const auto before = usage->used_bytes;
                auto moved = store_.migrateOnce(settings_.tiers[destination].config.name,
                                                settings_.tiers[source].config.rank,
                                                draining_[source] ? INT64_MAX : cutoff,
                                                static_cast<uint64_t>(tokens_));
                if(!moved.ok() || !*moved)
                    break;
                usage = store_.tierUsage(settings_.tiers[source].config.name);
                if(!usage.ok() || !usage->available)
                    break;
                tokens_ = std::max(0.0, tokens_ - static_cast<double>(before - std::min(before, usage->used_bytes)));
                if(Usage(settings_.tiers[source], *usage) < settings_.tiers[source].low_watermark)
                    draining_[source] = false;
            }
        }
    if(stopped_)
        return absl::CancelledError("migration worker stopped");
    if(!store_.migrationStopped())
        (void)store_.cleanupMigrations();
    (void)store_.sweepTiers();
    if(!store_.migrationStopped())
        (void)store_.writeTierReplicas();
    if(settings_.status_file.empty())
        return absl::OkStatus();
    nlohmann::json status{{"writer", settings_.writer},
                          {"migrate_enabled", settings_.enabled},
                          {"migration_stopped", store_.migrationStopped()},
                          {"tiers", nlohmann::json::array()},
                          {"pending_tier_deletions", nlohmann::json::object()}};
    for(const auto& tier: settings_.tiers)
    {
        auto usage = store_.tierUsage(tier.config.name);
        const FileTierStore::TierUsage value = usage.ok() ? *usage : FileTierStore::TierUsage{};
        status["tiers"].push_back({{"name", tier.config.name},
                                   {"rank", tier.config.rank},
                                   {"available", value.available},
                                   {"used_bytes", value.used_bytes},
                                   {"budget_bytes", tier.budget_bytes},
                                   {"above_high", value.available && Usage(tier, value) > tier.high_watermark}});
    }
    for(const auto& [story, count]: store_.pendingTierDeletions())
        status["pending_tier_deletions"][std::to_string(story)] = count;
    const auto target = std::filesystem::absolute(settings_.status_file).lexically_normal();
    auto parent = std::make_unique<tier_detail::Fd>(::open("/", O_DIRECTORY | O_RDONLY | O_CLOEXEC));
    for(const auto& component: target.parent_path().relative_path())
    {
        auto next = std::make_unique<tier_detail::Fd>(
                ::openat(parent->get(), component.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if(next->get() < 0)
            return tier_detail::IoError("open tier status parent");
        parent = std::move(next);
    }
    const auto name = target.filename().string();
    const auto temporary = name + ".tmp";
    tier_detail::Fd fd(
            ::openat(parent->get(), temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
    if(fd.get() < 0)
        return tier_detail::IoError("open tier status temporary");
    auto result = tier_detail::WriteAll(fd.get(), status.dump() + "\n");
    if(result.ok() && ::fsync(fd.get()) != 0)
        result = tier_detail::IoError("sync tier status");
    if(result.ok() && ::renameat(parent->get(), temporary.c_str(), parent->get(), name.c_str()) != 0)
        result = tier_detail::IoError("rename tier status");
    if(result.ok() && ::fsync(parent->get()) != 0)
        result = tier_detail::IoError("sync tier status parent");
    return result;
}
} // namespace chronolog::grapher
