
#include "chrono_monitor.h"

#include "PlaybackService.h"
#include "StoryChunkSender.h"

namespace tl = thallium;
namespace chl = chronolog;

chronolog::PlaybackService::PlaybackService(tl::engine &tl_engine, uint16_t service_provider_id
    , chronolog::ArchiveReadingRequestQueue & archive_reading_queue
    , chronolog::PlayerDataStore & data_store)
            : tl::provider <PlaybackService>(tl_engine, service_provider_id)
            , playbackEngine(tl_engine)
            , theArchiveReadingRequestQueue(archive_reading_queue)
            , theDataStore(data_store)
{
        define("playback_service_available", &PlaybackService::playback_service_available);
        define("story_playback_request", &PlaybackService::story_playback_request);

        //set up callback for the case when the engine is being finalized while this provider is still alive
        playbackEngine.push_finalize_callback(this, [p = this]()
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
        playbackEngine.pop_finalize_callback(this);
}

//////////////////

void chronolog::PlaybackService::playback_service_available(tl::request const &request)
{
        request.respond(1);
}

void chronolog::PlaybackService::story_playback_request(tl::request const &request
    , std::string const &chronicle_name, std::string const &story_name
    , uint64_t start_time, uint64_t end_time, chronolog::ServiceId const & receiver_service_id)
{
        LOG_INFO("[PlaybackService] PlaybackStoryRequest: ChronicleName={},StoryName={}", chronicle_name, story_name);

   //ChronoPlayer is running and able to respond 
   // generate unique RequestId (service_provider_id + atomic query index)
   uint32_t requestId = 1;
       
    chl::StoryChunkSender * storyChunkSender = nullptr;
    // if we already have StorychunkSender & ExtractionQueue for this receiver,
    // use it or add one otherwise
    {
        std::lock_guard<std::mutex> lock(playbackServiceMutex);

        auto findSenderIter = chunkSenders.find(receiver_service_id.get_service_endpoint());
        if(findSenderIter != chunkSenders.end())
        {
            storyChunkSender = (*findSenderIter).second;
        }
        else
        {
        //create RDMA client of the requesting service 
        // using the service tl_engine and service_id provided in the request
        storyChunkSender = chl::StoryChunkSender::CreateStoryChunkSender(playbackEngine, receiver_service_id);
        chunkSenders.insert(std::pair<chl::service_endpoint, chl::StoryChunkSender*>(receiver_service_id.get_service_endpoint(),storyChunkSender));

        }
    }

    // put new archiveRequest tied to the Sender's extractionQueue on 
    // onto the ArchiveReadingRequestQueue

    theArchiveReadingRequestQueue.pushReadingRequest(
            chl::ArchiveReadingRequest( &(storyChunkSender->getExtractionQueue()),chronicle_name, story_name, start_time, end_time)
        );
    
    // return requestId 
    request.respond(requestId);
}


