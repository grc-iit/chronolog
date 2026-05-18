#ifndef PLAYBACK_QUERY_RESPONSE_H
#define PLAYBACK_QUERY_RESPONSE_H

#include <vector>

#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>

#include <chronolog_types.h>

namespace chronolog
{

// Wire format for the replay query response sent from ChronoPlayer back to a
// ChronoLog client. Carries a flat, ordered vector of LogEvent values — the
// same type used internally on the server — without the StoryChunk envelope
// (metadata, map keying, revision tracking) that used to be on the wire.
struct PlaybackQueryResponse
{
    std::vector<LogEvent> events;

    template <typename Archive>
    void serialize(Archive& ar)
    {
        ar & events;
    }
};

} // namespace chronolog

#endif
