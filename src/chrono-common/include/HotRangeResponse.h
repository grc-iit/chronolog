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
// events and unconfirmed_events are the events the keeper retains in the
// range, split by whether the grapher acknowledged the chunk holding them.
// known_W and hot_floor decide where the player splits the replay between
// the archive and the keepers; see HotRangeSplit.h.
struct HotRangeResponse
{
    std::vector<LogEvent> events;             // ascending EventSequence order
    std::vector<LogEvent> unconfirmed_events; // ascending EventSequence order
    uint64_t hot_floor = UINT64_MAX;          // oldest retained tick; UINT64_MAX if none retained
    uint64_t known_W = 0;                     // keeper's last-seen persisted watermark (0 if none)
    bool truncated = false;                   // max_events cap hit; caller may re-request with a higher start

    template <typename SerArchiveT>
    void serialize(SerArchiveT& serT)
    {
        serT & events;
        serT & unconfirmed_events;
        serT & hot_floor;
        serT & known_W;
        serT & truncated;
    }
};

} // namespace chronolog

#endif // CHRONOLOG_HOT_RANGE_RESPONSE_H
