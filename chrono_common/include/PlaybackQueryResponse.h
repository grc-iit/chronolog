#ifndef PLAYBACK_QUERY_RESPONSE_H
#define PLAYBACK_QUERY_RESPONSE_H

#include <cstdint>
#include <string>
#include <vector>

#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>

namespace chronolog
{

// Wire format for the replay query response sent from ChronoPlayer back to a
// ChronoLog client. Carries exactly the fields needed to reconstruct a public
// chronolog::Event, with no dependency on the internal LogEvent / StoryChunk
// representation. This keeps StoryChunk machinery out of the client SDK.
//
// Field order is the serialization order; do not reorder without bumping the
// wire contract on both the Player and the Client.
struct WireEvent
{
    uint64_t eventTime;
    uint64_t clientId;
    uint32_t eventIndex;
    std::string logRecord;

    template <typename Archive>
    void serialize(Archive& ar)
    {
        ar & eventTime;
        ar & clientId;
        ar & eventIndex;
        ar & logRecord;
    }
};

// Top-level response body. A vector preserves the (eventTime, clientId,
// eventIndex) ordering produced by the Player without imposing a map's
// per-entry overhead on the wire.
struct PlaybackQueryResponse
{
    std::vector<WireEvent> events;

    template <typename Archive>
    void serialize(Archive& ar)
    {
        ar & events;
    }
};

} // namespace chronolog

#endif
