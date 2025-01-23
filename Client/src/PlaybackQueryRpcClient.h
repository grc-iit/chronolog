#ifndef PLAYBACK_QUERY_RPC_CLIENT_H
#define PLAYBACK_QUERY_RPC_CLIENT_H

#include <thallium.hpp>

#include "ServiceId.h"
#include "chronolog_types.h"
#include "chrono_monitor.h"

namespace tl= thallium;

namespace chronolog
{

class PlaybackQueryRpcClient
{
public:

    // Service should be created on the heap not the stack thus the constructor is private...
    static PlaybackQueryRpcClient*
    CreatePlaybackQueryRpcClient(tl::engine &tl_engine, ServiceId const& playback_service_id)
    {
        try
        {
            return new PlaybackQueryRpcClient(tl_engine, playback_service_id);
        }
        catch(tl::exception const &ex)
        {
            LOG_ERROR("[PlaybackQueryRpcClient] Failed to create PlaybackQueryRpcClient for {}", to_string(playback_service_id));
        }

        return nullptr;
    }

    ~PlaybackQueryRpcClient();

    int is_playback_service_available();

    int send_story_playback_request(ServiceId const & response_service_id, uint32_t query_id 
             , ChronicleName const & chronicle_name, StoryName const & story_name, uint64_t start_time, uint64_t end_time);

private:

    PlaybackQueryRpcClient() = delete;
    PlaybackQueryRpcClient(PlaybackQueryRpcClient const &) = delete;
    PlaybackQueryRpcClient &operator=(PlaybackQueryRpcClient const &) = delete;

    tl::engine & local_engine;          // local tl::engine
    ServiceId   playback_service_id;    // ServiceId of remote PlaybackService
    tl::provider_handle playback_service_handle;  // tl::provider_handle for remote PlaybackService
    tl::remote_procedure playback_service_available;
    tl::remote_procedure story_playback_request;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    PlaybackQueryRpcClient(tl::engine &tl_engine, ServiceId const& playback_service_id);

};

}

#endif
