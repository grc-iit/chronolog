#include <chrono>
#include <map>
#include <mutex>
#include <utility>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <PlaybackService.h>
#include <QueryResponseTransferAgent.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::PlaybackService::PlaybackService(tl::engine& tl_engine,
                                            uint16_t service_provider_id,
                                            chronolog::ArchiveReadingRequestQueue& archive_reading_queue)
    : tl::provider<PlaybackService>(tl_engine, service_provider_id)
    , playbackEngine(tl_engine)
    , theArchiveReadingRequestQueue(archive_reading_queue)
{
    define("playback_service_available", &PlaybackService::playback_service_available);
    define("story_playback_request", &PlaybackService::story_playback_request);

    //set up callback for the case when the engine is being finalized while this provider is still alive
    playbackEngine.push_finalize_callback(this, [p = this]() { delete p; });

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

void chronolog::PlaybackService::playback_service_available(tl::request const& request) { request.respond(1); }

/////////

void chronolog::PlaybackService::story_playback_request(tl::request const& request,
                                                        chl::ServiceId const& receiver_service_id,
                                                        uint32_t query_id,
                                                        chl::ChronicleName const& chronicle_name,
                                                        chl::StoryName const& story_name,
                                                        chl::chrono_time const& start_time,
                                                        chl::chrono_time const& end_time)
{
    LOG_INFO("[PlaybackService] story_playback_request for receiver_service {} Story {}-{}",
             chl::to_string(receiver_service_id),
             chronicle_name,
             story_name);

    //ChronoPlayer is running and able to respond

    chl::StoryChunkTransferAgent* storyChunkSender = nullptr;
    // if we already have StoryChunkTransferAgent & ExtractionQueue for this receiver,
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
            storyChunkSender =
                    chl::StoryChunkTransferAgent::CreateStoryChunkTransferAgent(playbackEngine, receiver_service_id);
            chunkSenders.insert(std::pair<chl::service_endpoint, chl::StoryChunkTransferAgent*>(
                    receiver_service_id.get_service_endpoint(),
                    storyChunkSender));
            storyChunkSender->startExtractionThreads(1);
        }
    }

    //chl::chrono_time active_window_boundary = PlayerDataStore.get_access_window_boundary();
    chl::chrono_time active_window_boundary = 1;

    // allocate PlaybackQueryResponse instance for this query 
    // and put it on the ResponseTransferAgentQueue
    chl::PlaybackQueryResponse * query_response = storyChunkSender->createQueryResponse(query_id);

    // handle the active in-memory portion of the query response
    if(start_time < active_window_boundary)
    {  
	 //PlayerDataStore.get_active_story_events( query_response.events());
    }


    if(end_time > active_window_boundary)
    {
    // put new archiveRequest tied to the Sender's extractionQueue on
    // onto the ArchiveReadingRequestQueue

        theArchiveReadingRequestQueue.pushReadingRequest(
            chl::ArchiveReadingRequest(storyChunkSender,
			               query_id,  
                                       chronicle_name,
                                       story_name,
                                       start_time,
                                       end_time));
    }

    request.respond(query_id);
}
