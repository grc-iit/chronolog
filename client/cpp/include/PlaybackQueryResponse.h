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
    // false when the player knows the series is short: a keeper of the story
    // did not answer, a keeper's answer hit its event cap, or an archive file
    // in the range could not be read. The client reports CL_ERR_PARTIAL_RESULT.
    bool complete = true;

    template <typename SerArchiveT>
    void serialize(SerArchiveT& serT)
    {
        serT & query_id;
        serT & events;
        serT & complete;
    }
};

} // namespace chronolog

#endif
