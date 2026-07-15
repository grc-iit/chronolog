#ifndef CHRONOLOG_HOT_RANGE_RESPONSE_H
#define CHRONOLOG_HOT_RANGE_RESPONSE_H

#include <cstdint>
#include <vector>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>

#include <chronolog_types.h>

namespace chronolog
{

// Response of the keeper's story_range_fetch RPC (the player's on-demand hot
// source for replay). Shared header: the keeper serializes it, the player
// deserializes it.
//
// hot_floor is the split-boundary ingredient: the oldest event tick this
// keeper still retains for the story. The retention store frees a chunk only
// once its events are durable in the archive and frees proceed oldest-first,
// so everything below hot_floor is guaranteed on disk. The player takes
// B = min(hot_floor) over the story's keepers and reads [start, min(end, B))
// from the archive — a completeness argument, not an optimization.
struct HotRangeResponse
{
    std::vector<LogEvent> events; // ascending EventSequence order
    uint64_t hot_floor = UINT64_MAX; // oldest retained tick; UINT64_MAX if none retained
    uint64_t known_W = 0;            // keeper's last-seen persisted watermark (0 if none)
    bool truncated = false;          // max_events cap hit; caller may re-request with a higher start

    template <typename SerArchiveT>
    void serialize(SerArchiveT& serT)
    {
        serT & events;
        serT & hot_floor;
        serT & known_W;
        serT & truncated;
    }
};

} // namespace chronolog

#endif // CHRONOLOG_HOT_RANGE_RESPONSE_H
