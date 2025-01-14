
#include "chrono_monitor.h"

#include "PlaybackService.h"
//#include "StoryChunkSender.h"

namespace tl = thallium;
namespace chl = chronolog;

chronolog::PlaybackService::PlaybackService(tl::engine &tl_engine, uint16_t service_provider_id
    , chronolog::ArchiveReadingAgent & reading_agent
    , chronolog::PlayerDataStore &data_store_instance)
            : tl::provider <PlaybackService>(tl_engine, service_provider_id)
            , theArchiveReadingAgent(reading_agent)
            , theDataStore(data_store_instance)
{
        define("playback_service_available", &PlaybackService::playback_service_available);
//        define("on_story_playback_request", &PlaybackService::on_story_playback_request);

        //set up callback for the case when the engine is being finalized while this provider is still alive
        get_engine().push_finalize_callback(this, [p = this]()
        { delete p; });

        std::stringstream ss;
        ss << get_engine().self();
        LOG_INFO("[PlaybackService] Constructed at {}. ProviderID={}", ss.str(), service_provider_id);
}
   
////////////////////

chronolog::PlaybackService::~PlaybackService()
{
        LOG_DEBUG("[PlaybackService] Destructor called. Cleaning up...");
        //remove provider finalization callback from the engine's list
        get_engine().pop_finalize_callback(this);
}

//////////////////

void chronolog::PlaybackService::playback_service_available(tl::request const &request)
{
        request.respond(1);
}

void chronolog::PlaybackService::on_story_playback_request(tl::request const &request
    , std::string const &chronicle_name, std::string const &story_name
    , uint64_t start_time, uint64_t end_time, chronolog::ServiceId const & requesting_service_id)
{
        LOG_INFO("[PlaybackService] PlaybackStoryRequest: ChronicleName={},StoryName={}", chronicle_name, story_name);

        //ChronoPlayer is running and able to respond 
        // generate unique RequestId (service_provider_id + atomic query index)
        uint32_t requestId = 1;
        
        //create RDMA client of the requesting service 
        // using the service tl_engine and service_id provided in the request
    
        //chl::StoryChunkSender * storyChunkSender = chl::StoryChunkSender::CreateStoryChunkSender(get_engine(), requesting_service_id);

        // put request onto the ArchiveREadingAgent requestQueue
        // and tie it to the RDMA Extractor's ExtractionQueue

        // return requestId 
        request.respond(requestId);
}


