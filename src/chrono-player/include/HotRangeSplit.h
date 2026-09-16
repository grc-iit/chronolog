#ifndef CHRONOLOG_HOT_RANGE_SPLIT_H
#define CHRONOLOG_HOT_RANGE_SPLIT_H

#include <cstdint>
#include <map>
#include <vector>

#include <chronolog_types.h>
#include <HotRangeResponse.h>

namespace chronolog
{

// Where a replay of [start, end) splits between the archive and the story's
// keepers, given every reachable keeper's hot-fetch response. The archive
// serves [start, B) and the keepers [B, end).
//
// B is the highest watermark any keeper reports, clamped to end. Each
// keeper's known_W is a watermark the grapher sent, so everything below B is
// on disk. A keeper frees a chunk only when its own known_W covers the chunk,
// and no known_W is above B, so every event at or above B is still held by
// its keeper. A lower B would not do: frees are per keeper, so a keeper can
// still hold an old chunk while another has freed newer ones, and a split at
// the old chunk leaves the newer freed events to nobody.
//
// When no keeper reports a watermark, no keeper has freed anything, and B is
// the lowest hot floor instead. A keeper that retains nothing, or whose fetch
// failed, reports known_W = 0 and hot_floor = UINT64_MAX and changes neither.
//
// Keeper events below B are in the archive, which returns them too, so they
// are dropped, except those in chunks the grapher has not confirmed written:
// the archive may not have those, whatever W is.
struct HotRangeSplit
{
    uint64_t boundary = 0;
    // keeper events at or above the boundary plus unconfirmed ones,
    // deduplicated across keepers, in EventSequence order
    std::vector<LogEvent> hotEvents;
    // the hot side reaches back to start: no archive read needed
    bool complete = false;
};

// What the player does with the rest of the replay: how far the archive read
// goes, and whether the reply it sends can claim to hold every event in range.
struct ReplayPlan
{
    bool archiveNeeded = false;
    uint64_t archiveEnd = 0;
    bool complete = true;
};

// A keeper that did not answer leaves its watermark unknown, so the events it
// has already freed can lie anywhere below the boundary the others reported.
// The archive read then covers the whole range instead of stopping at B, and
// the reply is short of whatever that keeper still held unwritten. A truncated
// answer costs the newest events, which the archive cannot serve, so it only
// marks the reply incomplete.
inline ReplayPlan planReplay(HotRangeSplit const& split,
                             uint64_t start_time,
                             uint64_t end_time,
                             bool all_keepers_answered,
                             bool any_truncated)
{
    ReplayPlan plan;
    plan.complete = all_keepers_answered && !any_truncated;
    if(!all_keepers_answered)
    {
        plan.archiveNeeded = true;
        plan.archiveEnd = end_time;
        return plan;
    }
    plan.archiveNeeded = !split.complete;
    plan.archiveEnd = split.boundary;
    return plan;
}

inline HotRangeSplit splitHotRange(std::vector<HotRangeResponse>& responses, uint64_t start_time, uint64_t end_time)
{
    HotRangeSplit split;
    uint64_t highest_watermark = 0;
    uint64_t lowest_floor = UINT64_MAX;
    for(auto const& response: responses)
    {
        if(response.known_W > highest_watermark)
        {
            highest_watermark = response.known_W;
        }
        if(response.hot_floor < lowest_floor)
        {
            lowest_floor = response.hot_floor;
        }
    }
    split.boundary = (highest_watermark > 0) ? highest_watermark : lowest_floor;
    if(split.boundary > end_time)
    {
        split.boundary = end_time;
    }

    std::map<EventSequence, LogEvent> merged; // cross-keeper dedup
    for(auto& response: responses)
    {
        for(auto& log_event: response.unconfirmed_events)
        {
            merged.emplace(EventSequence{log_event.time(), log_event.clientId, log_event.index()},
                           std::move(log_event));
        }
        for(auto& log_event: response.events)
        {
            if(log_event.time() >= split.boundary)
            {
                merged.emplace(EventSequence{log_event.time(), log_event.clientId, log_event.index()},
                               std::move(log_event));
            }
        }
    }
    for(auto& sequenced_event: merged) { split.hotEvents.push_back(std::move(sequenced_event.second)); }
    split.complete = (split.boundary <= start_time);
    return split;
}

} // namespace chronolog

#endif // CHRONOLOG_HOT_RANGE_SPLIT_H
