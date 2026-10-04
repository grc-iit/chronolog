#include "chrono-player/replay/HotReplay.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sys/vfs.h>

namespace
{
using namespace chronolog;
using namespace chronolog::player;
class Source final: public HotSource
{
public:
    absl::StatusOr<HotFetch> fetch(StoryId, const Range&) const override
    {
        HotFetch result;
        result.route_epoch = 1;
        result.archived_below = {300, 0};
        result.keepers = {{{"archive", 1, {300, 0}, true, false, {300, 0}}, {}}};
        return result;
    }
};
bool measure(const std::filesystem::path& root, const TierChain& chain, const char* name)
{
    std::vector<double> times;
    for(size_t iteration = 0; iteration < 100; ++iteration)
    {
        auto opened =
                FileTierStore::OpenReadOnly(root, std::chrono::hours(1), {}, 0, {}, std::chrono::seconds(30), chain);
        if(!opened.ok())
        {
            std::cerr << opened.status() << '\n';
            return false;
        }
        if(!(*opened)->probeTiers().ok())
            return false;
        HotReplayOptions options;
        options.archive = std::shared_ptr<FileTierStore>(*std::move(opened));
        options.batch_size = 1;
        HotReplay replay(std::make_shared<Source>(), options);
        const auto begin = std::chrono::steady_clock::now();
        auto stream = replay.read(1, {Range::Axis::Hlc, {100, 0}, {300, 0}});
        if(!stream.ok())
            return false;
        auto first = (*stream)->next();
        if(!first.ok() || !*first || (**first).events.empty())
            return false;
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
        size_t events = (**first).events.size();
        bool complete = (**first).completion && (**first).completion->complete;
        for(size_t batch = 0; batch < 10; ++batch)
        {
            auto next = (*stream)->next();
            if(!next.ok())
                return false;
            if(!*next)
                break;
            events += (**next).events.size();
            if((**next).completion)
                complete = (**next).completion->complete;
        }
        if(events != 2 || !complete)
            return false;
    }
    std::sort(times.begin(), times.end());
    std::cout << name << " reads=100 p50_ms=" << (times[49] + times[50]) / 2 << " max_ms=" << times.back()
              << " client_cache=included\n";
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if(argc != 3)
        return 2;
    const std::filesystem::path root = argv[1], slow = argv[2];
    auto opened = FileTierStore::Open(root, "tier-bench", {{1, {100, 0}}});
    if(!opened.ok())
        return 1;
    auto writer = *std::move(opened);
    for(int i = 0; i < 2; ++i)
    {
        Event event;
        event.id = {1, 2, 3, static_cast<uint64_t>(i + 1)};
        event.hlc = {120 + i * 100, 0};
        event.physical = {event.hlc.physical_ns, 0, ClockStatus::Synced};
        event.envelope.payload = std::string(65536, 'x');
        if(!writer->publish({std::to_string(i), 1, {100 + i * 100, 0}, {200 + i * 100, 0}, {event}}).ok())
            return 1;
    }
    CompactionPolicy policy;
    policy.min_files = 2;
    policy.min_age = std::chrono::seconds(0);
    policy.io_bytes_per_sec = policy.io_burst_bytes = 1ULL << 30;
    auto compacted = writer->compactOnce(policy);
    if(!compacted.ok() || compacted->inputs != 2)
        return 1;
    TierChain chain{"tier-bench",
                    {{"local", "posix", root, 0, "local-bench-uuid"}, {"slow", "posix", slow, 1, "slow-bench-uuid"}}};
    for(const auto& tier: chain.tiers)
    {
        struct statfs info
        {
        };
        if(::statfs(tier.root.c_str(), &info) != 0)
            return 1;
        std::ofstream(tier.root / ".chronolog-tier.json") << nlohmann::json{{"deployment_id", chain.deployment_id},
                                                                            {"name", tier.name},
                                                                            {"rank", tier.rank},
                                                                            {"kind", "posix"},
                                                                            {"tier_uuid", tier.tier_uuid},
                                                                            {"f_type", info.f_type}};
    }
    if(!measure(root, chain, "local"))
        return 1;
    if(!writer->scrubOnce(0).ok())
        return 1;
    if(!writer->configureTiers(chain.deployment_id, {chain.tiers[1]}).ok() || !writer->probeTiers().ok())
        return 1;
    auto migrated = writer->migrateOnce("slow");
    if(!migrated.ok() || *migrated != 1)
        return 1;
    return measure(root, chain, "slow") ? 0 : 1;
}
