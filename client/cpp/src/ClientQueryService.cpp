#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <thallium.hpp>
#include <thallium/serialization/stl/vector.hpp>

#include <client_errcode.h>
#include <chrono_monitor.h>
#include <chronolog_client.h>
#include <PlaybackQueryResponse.h>

#include "ClientQueryService.h"
#include "PlaybackQueryRpcClient.h"

namespace tl = thallium;
namespace chl = chronolog;


chl::ClientQueryService::ClientQueryService(thallium::engine& tl_engine, chl::ServiceId const& client_service_id)
    : tl::provider<ClientQueryService>(tl_engine, client_service_id.getProviderId())
    , queryServiceEngine(tl_engine)
    , queryServiceId(client_service_id)
    , queryTimeoutInSecs(180) // 3 mins
{
    std::atomic_init(&queryIndex, 0);

    LOG_DEBUG("[ClientQueryService] created  service {}", chl::to_string(queryServiceId));

    define("receive_story_chunk", &ClientQueryService::receive_story_chunk, tl::ignore_return_value());
    //set up callback for the case when the engine is being finalized while this provider is still alive
    get_engine().push_finalize_callback(this, [p = this]() { delete p; });
}


chl::ClientQueryService::~ClientQueryService()
{
    LOG_DEBUG("[ClientQueryService] Destructor called. Cleaning up...");
    get_engine().pop_finalize_callback(this);
}


///////////////
int chl::ClientQueryService::addStoryReader(ChronicleName const& chronicle,
                                            StoryName const& story,
                                            ServiceId const& service_id)
{
    chl::PlaybackQueryRpcClient* playbackRpcClient = addPlaybackQueryClient(service_id);

    if(nullptr == playbackRpcClient)
    {
        return chl::CL_ERR_UNKNOWN;
    }

    std::lock_guard<std::mutex> lock(queryServiceMutex);
    auto insert_return = acquiredStoryMap.insert(
            std::pair<std::pair<chl::ChronicleName, chl::StoryName>, chl::PlaybackQueryRpcClient*>(
                    std::pair<chl::ChronicleName, chl::StoryName>(chronicle, story),
                    playbackRpcClient));

    return (insert_return.second ? chl::CL_SUCCESS : chl::CL_ERR_UNKNOWN);
}

void chl::ClientQueryService::removeStoryReader(chl::ChronicleName const& chronicle, chl::StoryName const& story)
{
    std::lock_guard<std::mutex> lock(queryServiceMutex);
    acquiredStoryMap.erase(std::pair<chl::ChronicleName, chl::StoryName>(chronicle, story));
}

////////////

int chl::ClientQueryService::replay_story(chl::ChronicleName const& chronicle,
                                          chl::StoryName const& story,
                                          uint64_t start,
                                          uint64_t end,
                                          std::vector<chl::Event>& event_series)
{

    //check if the story has been acquired and the chrono_player is available for it

    auto storyReader_iter = acquiredStoryMap.find(std::pair<chl::ChronicleName, chl::StoryName>(chronicle, story));

    if(storyReader_iter == acquiredStoryMap.end())
    {
        return chl::CL_ERR_NOT_ACQUIRED;
    }

    PlaybackQueryRpcClient* playbackRpcClient = (*storyReader_iter).second;

    // instantiate new query object

    auto timeout_time =
            (std::chrono::steady_clock::now() + std::chrono::seconds(queryTimeoutInSecs)).time_since_epoch().count();

    chl::PlaybackQuery* query = start_query(timeout_time, chronicle, story, start, end, event_series);

    if(query == nullptr)
    {
        return chl::CL_ERR_UNKNOWN;
    }

    //TODO: check that rpcQueryClient object can not be destroyed while we make the RPC call...

    //send query request to the appropriate chrono_player PlaybackService
    if((playbackRpcClient == nullptr) ||
       (playbackRpcClient->send_story_playback_request(query->queryId, chronicle, story, start, end) !=
        chl::CL_SUCCESS))
    {
        stop_query(query->queryId);
        return chl::CL_ERR_NO_PLAYERS;
    }

    // now wait for the asynchronous response with periodic polling of the query status
    // response will be received on a different thread

    uint64_t current_time = std::chrono::steady_clock::now().time_since_epoch().count();
    while(!query->completed && current_time < query->timeout_time)
    {
        sleep(1); //TODO: replace with finer granularity sleep...
        current_time = std::chrono::steady_clock::now().time_since_epoch().count();
    }

    int ret_value = (query->completed == true ? chl::CL_SUCCESS : chl::CL_ERR_QUERY_TIMED_OUT);

    // destroy query object and return to the caller
    stop_query(query->queryId);

    return ret_value;
}
//////

chl::PlaybackQuery* chl::ClientQueryService::start_query(uint64_t timeout_time,
                                                         chl::ChronicleName const& chronicle,
                                                         chl::StoryName const& story,
                                                         chl::chrono_time const& start_time,
                                                         chl::chrono_time const& end_time,
                                                         std::vector<chl::Event>& playback_events)
{
    std::lock_guard<std::mutex> lock(queryServiceMutex);

    uint32_t query_id = queryIndex++;

    //TODO add query_id to queryResponse object  and remove this line...
    query_id = 1;
    auto insert_return = activeQueryMap.insert(std::pair<uint32_t, chl::PlaybackQuery>(
            query_id,
            chl::PlaybackQuery(playback_events, query_id, timeout_time, chronicle, story, start_time, end_time)));

    if(insert_return.second)
    {
        return &(*insert_return.first).second;
    }
    else
    {
        return nullptr;
    }
}

void chl::ClientQueryService::stop_query(uint32_t query_id)
{
    std::lock_guard<std::mutex> lock(queryServiceMutex);

    activeQueryMap.erase(query_id);
}
////

// find or create PlaybackServiceRpcClient associated with the remote Playback Service
chl::PlaybackQueryRpcClient* chronolog::ClientQueryService::addPlaybackQueryClient(chl::ServiceId const& player_card)
{
    LOG_DEBUG("[ClientQueryService] adding PlaybackQueryRpcClient for {}", chl::to_string(player_card));


    auto find_iter = playbackRpcClientMap.find(player_card.get_service_endpoint());

    if(find_iter != playbackRpcClientMap.end())
    {
        LOG_DEBUG("[ClientQueryService] found PlaybackQueryRpcClient for {}", chl::to_string(player_card));
        return (*find_iter).second;
    }

    chl::PlaybackQueryRpcClient* playbackRpcClient = nullptr;
    LOG_DEBUG("[ClientQueryService] adding PlaybackQueryRpcClient for {}", chl::to_string(player_card));

    std::lock_guard<std::mutex> lock(queryServiceMutex);

    try
    {
        playbackRpcClient = chronolog::PlaybackQueryRpcClient::CreatePlaybackQueryRpcClient(*this, player_card);

        auto insert_return = playbackRpcClientMap.insert(
                std::pair<chl::service_endpoint, chl::PlaybackQueryRpcClient*>(player_card.get_service_endpoint(),
                                                                               playbackRpcClient));

        if(false != insert_return.second)
        {
            LOG_DEBUG("[ClientQueryService] created PlaybackQueryRpcClient for {}", chl::to_string(player_card));
            return playbackRpcClient;
        }
        else
        {
            delete playbackRpcClient;
            playbackRpcClient = nullptr;
            LOG_DEBUG("[ClientQueryService] Failed to create PlaybackQueryRpcClient}", chl::to_string(player_card));
        }
    }
    catch(tl::exception const& ex)
    {
        playbackRpcClient = nullptr;
        LOG_DEBUG("[ClientQueryService] Failed to create PlaybackQueryRpcClient}", chl::to_string(player_card));
    }
    return playbackRpcClient;
}

// we are notified of the remote PlaybackService stopping, remove associated PlaybackQueryRpcClient
void chronolog::ClientQueryService::removePlaybackQueryClient(chl::ServiceId const& player_card)
{
    std::lock_guard<std::mutex> lock(queryServiceMutex);
    //TODO : we need to check that there no active queries associated with this PlaybackQueryRpcClient
    // to safely remove it
}

// Receive a PlaybackQueryResponse from the Player and append its events
// directly to the active query's event series. The RPC keeps the legacy name
// "receive_story_chunk" for now to avoid changing the Thallium endpoint name
// in this refactor; the wire payload is no longer a StoryChunk.
void chl::ClientQueryService::receive_story_chunk(tl::request const& request, tl::bulk& b)
{
    try
    {
        tl::endpoint ep = request.get_endpoint();
        LOG_DEBUG("[ClientQueryService] receive_story_chunk :Endpoint obtained, ThreadID={}", tl::thread::self_id());

        std::vector<char> mem_vec(b.size());
        std::vector<std::pair<void*, std::size_t>> segments(1);
        segments[0].first = (void*)(&mem_vec[0]);
        segments[0].second = mem_vec.size();
        LOG_DEBUG("[ClientQueryService] Bulk memory prepared, size: {}, ThreadID={}",
                  mem_vec.size(),
                  tl::thread::self_id());
        tl::engine local_engine = get_engine();

        tl::bulk local = local_engine.expose(segments, tl::bulk_mode::write_only);
        LOG_DEBUG("[ClientQueryService] Bulk memory exposed, ThreadID={}", tl::thread::self_id());
        b.on(ep) >> local;
        LOG_DEBUG("[ClientQueryService] Received {} bytes of PlaybackQueryResponse data, ThreadID={}",
                  b.size(),
                  tl::thread::self_id());

        chronolog::PlaybackQueryResponse response;
        int ret = deserializeResponse(&mem_vec[0], b.size(), response);
        if(ret != chronolog::CL_SUCCESS)
        {
            LOG_ERROR("[ClientQueryService] Failed to deserialize PlaybackQueryResponse, ThreadID={}",
                      tl::thread::self_id());
            ret = 10000000 + tl::thread::self_id(); // arbitrary error code encoded with thread id
            LOG_ERROR("[ClientQueryService] Discarding the response, responding {} to Player", ret);
            request.respond(ret);
            return;
        }

        LOG_DEBUG("[ClientQueryService] PlaybackQueryResponse received: eventCount {} ThreadID={}",
                  response.events.size(),
                  tl::thread::self_id());

        uint32_t query_id = 1; //TODO:  add query_id to query response transfer

        // NOTE: by design there would be only one receiving thread that's writing to the specific query object
        // but we probably should take care of the possibility of the query timeout happening while we are writing the response

        auto query_iter = activeQueryMap.find(query_id);
        if(query_iter != activeQueryMap.end())
        {
            std::vector<chl::Event>& event_series = (*query_iter).second.eventSeries;
            event_series.reserve(event_series.size() + response.events.size());
            for(auto const& log_event: response.events)
            {
                event_series.emplace_back(log_event.eventTime,
                                          log_event.clientId,
                                          log_event.eventIndex,
                                          log_event.logRecord);
            }
            (*query_iter).second.completed = true;
            LOG_DEBUG("[ClientQueryService] Query {} got {} events, ThreadID={}",
                      query_id,
                      response.events.size(),
                      tl::thread::self_id());
        }

        LOG_DEBUG("[ClientQueryService] PlaybackQueryResponse recording RPC response {}, ThreadID={}",
                  b.size(),
                  tl::thread::self_id());
        request.respond(b.size());
    }
    catch(std::bad_alloc const& ex)
    {
        LOG_ERROR("[ClientQueryService] Failed to allocate memory for PlaybackQueryResponse data, ThreadID={}",
                  tl::thread::self_id());
        request.respond(20000000 + tl::thread::self_id());
    }
}

int chl::ClientQueryService::deserializeResponse(char* buffer, size_t size, chl::PlaybackQueryResponse& response)
{
    std::stringstream ss(std::ios::binary | std::ios::in | std::ios::out);
    try
    {
        ss.write(buffer, size);
        cereal::BinaryInputArchive iarchive(ss);
        iarchive(response);
        return chronolog::CL_SUCCESS;
    }
    catch(cereal::Exception const& ex)
    {
        LOG_ERROR("[ClientQueryService] Failed to deserialize PlaybackQueryResponse, size={}, ThreadID={}. "
                  "Cereal exception: {}",
                  ss.str().size(),
                  tl::thread::self_id(),
                  ex.what());
    }
    catch(std::exception const& ex)
    {
        LOG_ERROR("[ClientQueryService] Failed to deserialize PlaybackQueryResponse, size={}, ThreadID={}. "
                  "std::exception : {}",
                  ss.str().size(),
                  tl::thread::self_id(),
                  ex.what());
    }
    catch(...)
    {
        LOG_ERROR("[ClientQueryService] Failed to deserialize PlaybackQueryResponse, ThreadID={}. Unknown exception "
                  "encountered.",
                  tl::thread::self_id());
    }
    return chronolog::CL_ERR_UNKNOWN;
}
