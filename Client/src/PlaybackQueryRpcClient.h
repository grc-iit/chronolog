#ifndef PLAYBACK_QUERY_RPC_CLIENT_H
#define PLAYBACK_QUERY_RPC_CLIENT_H

#include <thallium.hpp>

#include "ServiceId.h"
#include "chronolog_types.h"
#include "chrono_monitor.h"

#include "ClientQueryService.h"

namespace tl= thallium;

namespace chronolog
{

class PlaybackQueryRpcClient
{
public:

    // Service should be created on the heap not the stack thus the constructor is private...
    static PlaybackQueryRpcClient*
    CreatePlaybackQueryRpcClient( ClientQueryService & localQueryService, ServiceId const& playback_service_id)
    {
        try
        {
            return new PlaybackQueryRpcClient( localQueryService, playback_service_id);
        }
        catch(tl::exception const &ex)
        {
            LOG_ERROR("[PlaybackQueryRpcClient] Failed to create PlaybackQueryRpcClient for {}", to_string(playback_service_id));
        }

        return nullptr;
    }

    ~PlaybackQueryRpcClient();

    int is_playback_service_available();

    int send_story_playback_request(ChronicleName const & chronicle_name, StoryName const & story_name, uint64_t start_time, uint64_t end_time);

private:

    PlaybackQueryRpcClient() = delete;
    PlaybackQueryRpcClient(PlaybackQueryRpcClient const &) = delete;
    PlaybackQueryRpcClient &operator=(PlaybackQueryRpcClient const &) = delete;

    ClientQueryService & theClientQueryService; // ClientQueryService instance
    ServiceId   playback_service_id;    // ServiceId of remote PlaybackService
    tl::provider_handle playback_service_handle;  // tl::provider_handle for remote PlaybackService
    tl::remote_procedure playback_service_available;
    tl::remote_procedure story_playback_request;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    PlaybackQueryRpcClient(ClientQueryService &, ServiceId const& playback_service_id);

};

}

#endif
