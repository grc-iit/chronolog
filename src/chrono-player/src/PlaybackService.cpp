#include <chrono>
#include <map>
#include <mutex>
#include <utility>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <ArchiveReadingRequestQueue.h>
#include <PlaybackService.h>
#include <PlayerDataStore.h>
#include <QueryResponseTransferAgent.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::PlaybackService::PlaybackService(tl::engine& tl_engine,
                                            uint16_t service_provider_id,
                                            chronolog::PlayerDataStore& activeDataStore,
                                            chronolog::ArchiveReadingRequestQueue& archive_reading_queue)
    : tl::provider<PlaybackService>(tl_engine, service_provider_id)
    , playbackEngine(tl_engine)
    , theActiveDataStore(activeDataStore)
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

    std::lock_guard<std::mutex> lock(playbackServiceMutex);
    for(auto& agent: responseSenders)
    {
        agent.second->stopResponseThreads();
        delete agent.second;
    }
    responseSenders.clear();

    //remove provider finalization callback from the engine's list
    playbackEngine.pop_finalize_callback(this);
}

//////////////////

void chronolog::PlaybackService::playback_service_available(tl::request const& request) { request.respond(1); }

/////////

void chronolog::PlaybackService::story_playback_request(tl::request const& request,
                                                        chl::ServiceId const& receiver_service_id,
                                                        uint32_t query_id,
                                                        chl::StoryId const& story_id,
                                                        chl::ChronicleName const& chronicle_name,
                                                        chl::StoryName const& story_name,
                                                        chl::chrono_time const& start_time,
                                                        chl::chrono_time const& end_time)
{
    LOG_INFO("[PlaybackService] story_playback_request for receiver_service {} query_id {} story {} {}-{}",
             chl::to_string(receiver_service_id),
             query_id,
             story_id,
             chronicle_name,
             story_name);

    //ChronoPlayer is running and able to respond

    chl::QueryResponseAgent* queryResponseSender = nullptr;
    // if we already have QueryResponseAgent for this receiver,
    // use it or add one otherwise
    {
        std::lock_guard<std::mutex> lock(playbackServiceMutex);

        auto findSenderIter = responseSenders.find(receiver_service_id.get_service_endpoint());
        if(findSenderIter != responseSenders.end())
        {
            queryResponseSender = (*findSenderIter).second;
        }
        else
        {
            //create RDMA client of the requesting service
            // using the service tl_engine and service_id provided in the request
            queryResponseSender =
                    chl::QueryResponseAgent::CreateQueryResponseAgent(playbackEngine, receiver_service_id);
            if(queryResponseSender == nullptr)
            {
                request.respond(0);
                return;
            }

            responseSenders.insert(std::pair<chl::service_endpoint, chl::QueryResponseAgent*>(
                    receiver_service_id.get_service_endpoint(),
                    queryResponseSender));
            queryResponseSender->startResponseThreads(1);
        }
    }

    chl::chrono_time active_window_boundary = theActiveDataStore.get_active_window_boundary();

    LOG_DEBUG("[PlaybackService] query_id {} story_id {} range {}-{} active_window_boundary {}",
              query_id,
              story_id,
              start_time,
              end_time,
              active_window_boundary);

    // allocate PlaybackQueryResponse instance for this query
    // and put it on the ResponseTransferAgent's active_queries map

    chl::PlaybackQueryResponse* query_response = new chl::PlaybackQueryResponse(query_id);

    // handle the active in-memory portion of the query response
    if(active_window_boundary < end_time)
    {
        // portion of the playback response is coming from
        // the active PlayerDataStore
        theActiveDataStore.get_active_story_events(
                story_id,
                (start_time < active_window_boundary ? active_window_boundary : start_time),
                end_time,
                query_response->events);

        LOG_DEBUG("[PlaybackService] query {} for story_id {} got {} events from active DataStore",
                  query_id,
                  story_id,
                  query_response->events.size());
    }

    bool response_is_complete = false;
    if(active_window_boundary <= start_time)
    {
        response_is_complete = true;
    }

    if(chl::CL_SUCCESS != queryResponseSender->stashQueryResponseRecord(query_id, query_response, response_is_complete))
    {
        delete query_response;
        request.respond(0);
        return;
    }

    if(!response_is_complete)
    {
        // end_time > active_window_boundary
        // portion of the playback response is coming from the archived files

        // create an archiveRequest and put it
        // onto the ArchiveReadingRequestQueue

        chl::ArchiveReadingRequest* a_request =
                new chl::ArchiveReadingRequest(queryResponseSender,
                                               query_id,
                                               chronicle_name,
                                               story_name,
                                               start_time,
                                               (end_time < active_window_boundary ? end_time : active_window_boundary));

        theArchiveReadingRequestQueue.pushReadingRequest(a_request);
    }

    request.respond(query_id);
}
