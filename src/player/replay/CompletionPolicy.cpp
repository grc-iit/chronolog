#include "chrono-player/replay/CompletionPolicy.h"
#include <algorithm>

namespace chronolog::player
{

Completion CompletionPolicy::decide(const Range& range,
                                    Epoch route_epoch,
                                    const std::vector<KeeperFrontier>& keepers,
                                    const std::vector<WriterAssignment>& writers,
                                    bool archive_failed,
                                    bool physical_policy,
                                    bool unbounded_event)
{
    const bool hlc = range.axis == Range::Axis::Hlc;
    auto failed = [&](const KeeperFrontier& k)
    { return !k.answered || k.epoch != (k.expected_epoch ? k.expected_epoch : route_epoch); };
    auto lagging = [&](const KeeperFrontier& k)
    {
        return hlc ? k.sealed < (k.predecessor ? std::min(range.end, k.own_cut) : range.end)
                   : physical_policy && (!k.physical_frontier || *k.physical_frontier < range.end.physical_ns);
    };

    // A Route with no Keepers is an uninitialized route, never vacuously complete.
    bool any_failed = archive_failed || keepers.empty();
    bool any_truncated = false, any_lagging = false;
    std::optional<Hlc> min_seal;
    for(const auto& k: keepers)
    {
        if(failed(k))
        {
            any_failed = true;
            continue;
        }
        any_truncated |= k.truncated;
        any_lagging |= lagging(k);
        if(!k.predecessor || k.sealed < k.own_cut)
            min_seal = min_seal ? std::min(*min_seal, k.sealed) : k.sealed;
    }

    Completion c;
    c.frontier = min_seal.value_or(Hlc{});
    if(any_failed)
        c.reason = IncompleteReason::SourceFailed;
    else if(any_truncated)
        c.reason = IncompleteReason::Truncated;
    else if(!hlc && (!physical_policy || unbounded_event))
        c.reason = IncompleteReason::PhysicalAxisUnbounded;
    else if(any_lagging)
        c.reason = IncompleteReason::LaggingWriters;
    c.complete = c.reason == IncompleteReason::None;

    for(const auto& w: writers)
    {
        auto it = std::find_if(keepers.begin(),
                               keepers.end(),
                               [&](const KeeperFrontier& k) { return k.process_id == w.keeper; });
        if(it == keepers.end())
            continue; // The directory names a Keeper outside the Route.
        if(failed(*it))
            c.laggards.push_back({w.writer_id, w.incarnation, Hlc{}});
        else if(lagging(*it))
            c.laggards.push_back({w.writer_id, w.incarnation, it->sealed});
    }
    return c;
}

} // namespace chronolog::player
