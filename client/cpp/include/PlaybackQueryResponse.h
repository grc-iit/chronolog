#ifndef PLAYBACK_QUERY_RESPONSE_H
#define PLAYBACK_QUERY_RESPONSE_H

#include <vector>

#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>

#include <chronolog_client.h>

namespace chronolog
{

typedef uint32_t ClientQueryId;

// Wire format for the replay query response sent from ChronoPlayer back to a
// ChronoLog client. Carries a flat, ordered vector of Event records

struct PlaybackQueryResponse
{
    std::vector<Event> events;

    template <typename Archive>
    void serialize(Archive& ar)
    {
        ar & events;
    }
};

} // namespace chronolog

#endif
