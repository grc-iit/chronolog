#ifndef PLAYBACK_SERVICE_H
#define PLAYBACK_SERVICE_H

#include <iostream>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "chronolog_types.h"
#include "ArchiveReadingAgent.h"
#include "PlayerDataStore.h"

namespace tl = thallium;

namespace chronolog
{

class PlaybackService: public tl::provider <PlaybackService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static PlaybackService*
    CreatePlaybackService(tl::engine &tl_engine, uint16_t service_provider_id, ArchiveReadingAgent & archiveReadingAgentInstance, PlayerDataStore &dataStoreInstance)
    {
        return new PlaybackService(tl_engine, service_provider_id, archiveReadingAgentInstance, dataStoreInstance);
    }

    ~PlaybackService()
    {
        LOG_DEBUG("[PlaybackService] Destructor called. Cleaning up...");
        //remove provider finalization callback from the engine's list
        get_engine().pop_finalize_callback(this);
    }

    void playback_service_available(tl::request const &request)
    {
        request.respond(1);
    }

    void
    OnStoryPlaybackRequest(tl::request const &request, std::string const &chronicle_name, std::string const &story_name
                        , uint64_t start_time, uint64_t end_time, ServiceId const & requesting_service_id)
    {
        LOG_INFO("[PlaybackService] PlaybackStoryRequest: ChronicleName={},StoryName={}", chronicle_name, story_name);

        //ChronoPlayer is running and able to respond 
        // generate unique RequestId
        uint32_t requestId = 1;

        //create RDMA client of the requesting service 
        // using the service tl_engine and service_id provided in the request

        // put request onto the ArchiveREadingAgent requestQueue
        // and tie it to the RDMA Extractor's ExtractionQueue

        // return requestId 
        request.respond(requestId);
    }


private:
    PlaybackService(tl::engine &tl_engine, uint16_t service_provider_id, ArchiveReadingAgent & reading_Agent_instance, PlayerDataStore &data_store_instance)
            : tl::provider <PlaybackService>(tl_engine, service_provider_id)
            , theArchiveReadingAgent(reading_agent_intance)
            , theDataStore(data_store_instance)
    {
        define("playback_service_available", &PlaybackService::playback_service_available);
        define("story_playback_request", &PlaybackService::OnStoryPlaybackRequest);

        //set up callback for the case when the engine is being finalized while this provider is still alive
        get_engine().push_finalize_callback(this, [p = this]()
        { delete p; });

        std::stringstream ss;
        ss << get_engine().self();
        LOG_INFO("[PlaybackService] Constructed at {}. ProviderID={}", ss.str(), service_provider_id);
    }

    PlaybackService(PlaybacService const &) = delete;

    PlaybackService &operator=(PlaybackService const &) = delete;

    ArchiveReadingAgent & theArchiveReadingAgent;
    PlayerDataStore & theDataStore;
};

}// namespace chronolog

#endif
