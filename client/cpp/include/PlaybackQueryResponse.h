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
    PlaybackQueryResponse(ClientQueryId const& client_query_id = 0)
        : query_id(client_query_id)
    {}

    ClientQueryId query_id;
    std::vector<Event> events;

    template <typename SerArchiveT>
    void serialize(SerArchiveT& serT)
    {
        serT & query_id;
        serT & events;
    }
};

} // namespace chronolog

#endif
