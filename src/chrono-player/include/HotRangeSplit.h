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
// keepers, given every reachable keeper's hot-fetch response.
//
// The boundary B is the minimum hot floor over the responses, clamped to end.
// Every keeper frees a chunk only once it is durable in the archive, and frees
// proceed oldest-first, so everything below B is guaranteed on disk: the
// archive serves [start, B) and the keepers [B, end). This is a completeness
// argument, not an optimization. A keeper retaining nothing, or one whose
// fetch failed, reports hot_floor = UINT64_MAX and drops out of the min; that
// only raises B, and archive overlap is the failure-safe direction.
struct HotRangeSplit
{
    uint64_t boundary = 0;
    // keeper events at or above the boundary, deduplicated across keepers, in
    // EventSequence order
    std::vector<LogEvent> hotEvents;
    // the hot side reaches back to start: no archive read needed
    bool complete = false;
};

inline HotRangeSplit splitHotRange(std::vector<HotRangeResponse>& responses, uint64_t start_time, uint64_t end_time)
{
    HotRangeSplit split;
    split.boundary = end_time;
    std::map<EventSequence, LogEvent> merged; // cross-keeper dedup
    for(auto& response: responses)
    {
        if(response.hot_floor < split.boundary)
        {
            split.boundary = response.hot_floor;
        }
        for(auto& log_event: response.events)
        {
            merged.emplace(EventSequence{log_event.time(), log_event.clientId, log_event.index()},
                           std::move(log_event));
        }
    }
    // events below the boundary are archive-covered: the archive portion of
    // the same query returns them, so keeping both would duplicate them
    for(auto& sequenced_event: merged)
    {
        if(sequenced_event.second.time() >= split.boundary)
        {
            split.hotEvents.push_back(std::move(sequenced_event.second));
        }
    }
    split.complete = (split.boundary <= start_time);
    return split;
}

} // namespace chronolog

#endif // CHRONOLOG_HOT_RANGE_SPLIT_H
