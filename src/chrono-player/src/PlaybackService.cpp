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

    {
        std::lock_guard<std::mutex> lock(hotFetchMutex);
        for(auto& client: hotFetchClients)
        {
            delete client.second;
        }
        hotFetchClients.clear();
    }

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

chronolog::KeeperHotFetchClient* chronolog::PlaybackService::getHotFetchClient(chl::ServiceId const& keeper_service_id)
{
    {
        std::lock_guard<std::mutex> lock(hotFetchMutex);
        auto client_iter = hotFetchClients.find(keeper_service_id.get_service_endpoint());
        if(client_iter != hotFetchClients.end())
        {
            return client_iter->second;
        }
    }
    // engine lookup happens outside the cache mutex
    KeeperHotFetchClient* client = KeeperHotFetchClient::CreateKeeperHotFetchClient(playbackEngine, keeper_service_id);
    if(client == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(hotFetchMutex);
    auto emplaced = hotFetchClients.emplace(keeper_service_id.get_service_endpoint(), client);
    if(!emplaced.second)
    {
        // another thread won the race; keep the cached one
        delete client;
    }
    return emplaced.first->second;
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

    // The hot portion of the response comes straight from the story's keepers
    // (on-demand pull over the roster the visor delivered at story start),
    // and the split boundary B = min over keepers of their hot floor — the
    // oldest tick each keeper still retains. Every keeper frees a chunk only
    // once it is durable in the archive and frees proceed oldest-first, so
    // everything below B is guaranteed on disk; this is a completeness
    // argument, not an optimization. A keeper retaining nothing reports
    // hot_floor=UINT64_MAX and drops out of the min; a keeper that fails to
    // respond does the same, which only raises B (archive overlap is the
    // failure-safe direction). No keepers at all -> archive-only (degraded,
    // correct for persisted data).
    constexpr uint64_t kHotFetchMaxEvents = 262144;

    std::vector<chl::ServiceId> story_keepers = theActiveDataStore.getStoryKeepers(story_id);

    uint64_t hot_boundary = end_time; // B, clamped to the requested range
    std::map<chl::EventSequence, chl::LogEvent> merged_hot_events; // cross-keeper dedup for free
    for(auto const& keeper_service_id: story_keepers)
    {
        chl::KeeperHotFetchClient* fetch_client = getHotFetchClient(keeper_service_id);
        if(fetch_client == nullptr)
        {
            continue; // unreachable keeper: drops out of the min, B only rises
        }
        chl::HotRangeResponse hot = fetch_client->fetchRange(story_id, start_time, end_time, kHotFetchMaxEvents);
        if(hot.truncated)
        {
            LOG_WARNING("[PlaybackService] query {} story {} hot fetch from {} truncated at {} events",
                        query_id,
                        story_id,
                        chl::to_string(keeper_service_id),
                        kHotFetchMaxEvents);
        }
        if(hot.hot_floor < hot_boundary)
        {
            hot_boundary = hot.hot_floor;
        }
        for(auto& log_event: hot.events)
        {
            merged_hot_events.emplace(chl::EventSequence{log_event.time(), log_event.clientId, log_event.index()},
                                      std::move(log_event));
        }
    }

    LOG_DEBUG("[PlaybackService] query_id {} story_id {} range {}-{} keepers {} hot_boundary {} hot_events {}",
              query_id,
              story_id,
              start_time,
              end_time,
              story_keepers.size(),
              hot_boundary,
              merged_hot_events.size());

    // allocate PlaybackQueryResponse instance for this query
    // and put it on the ResponseTransferAgent's active_queries map

    chl::PlaybackQueryResponse* query_response = new chl::PlaybackQueryResponse(query_id);

    // hot side: merged keeper events at or above B. Events below B are
    // dropped — they are guaranteed archive-covered (E <= W) and the archive
    // portion of this same query returns them; keeping both would duplicate
    // them in the response.
    for(auto const& sequenced_event: merged_hot_events)
    {
        chl::LogEvent const& log_event = sequenced_event.second;
        if(log_event.time() < hot_boundary)
        {
            continue;
        }
        query_response->events.push_back(
                chl::Event{log_event.eventTime, log_event.clientId, log_event.eventIndex, log_event.logRecord});
    }

    LOG_DEBUG("[PlaybackService] query {} for story_id {} got {} hot events from {} keeper(s)",
              query_id,
              story_id,
              query_response->events.size(),
              story_keepers.size());

    // archive side covers [start_time, B); complete without it only when the
    // hot side reaches back to start_time
    bool response_is_complete = (hot_boundary <= start_time);

    if(chl::CL_SUCCESS != queryResponseSender->stashQueryResponseRecord(query_id, query_response, response_is_complete))
    {
        delete query_response;
        request.respond(0);
        return;
    }

    if(!response_is_complete)
    {
        // portion of the playback response is coming from the archived files

        // create an archiveRequest and put it
        // onto the ArchiveReadingRequestQueue

        chl::ArchiveReadingRequest* a_request = new chl::ArchiveReadingRequest(queryResponseSender,
                                                                               query_id,
                                                                               chronicle_name,
                                                                               story_name,
                                                                               start_time,
                                                                               hot_boundary);

        theArchiveReadingRequestQueue.pushReadingRequest(a_request);
    }

    request.respond(query_id);
}
