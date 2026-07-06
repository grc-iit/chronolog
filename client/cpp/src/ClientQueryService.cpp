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

    define("receive_query_response", &ClientQueryService::receive_query_response, tl::ignore_return_value());
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
                                            StoryId const& story_id,
                                            ServiceId const& service_id)
{
    chl::PlaybackQueryRpcClient* playbackRpcClient = addPlaybackQueryClient(service_id);

    if(nullptr == playbackRpcClient)
    {
        return chl::CL_ERR_UNKNOWN;
    }

    std::lock_guard<std::mutex> lock(queryServiceMutex);
    auto insert_return = acquiredStoryMap.insert(std::pair<std::pair<chl::ChronicleName, chl::StoryName>,
                                                           std::pair<chl::StoryId, chl::PlaybackQueryRpcClient*>>(
            std::pair<chl::ChronicleName, chl::StoryName>(chronicle, story),
            std::pair<chl::StoryId, chl::PlaybackQueryRpcClient*>(story_id, playbackRpcClient)));

    return (insert_return.second ? chl::CL_SUCCESS : chl::CL_ERR_UNKNOWN);
}

void chl::ClientQueryService::removeStoryReader(chl::ChronicleName const& chronicle, chl::StoryName const& story)
{
    std::lock_guard<std::mutex> lock(queryServiceMutex);
    acquiredStoryMap.erase(std::pair<chl::ChronicleName, chl::StoryName>(chronicle, story));
}

////////////

// Common dispatch loop shared by both replay_story overloads. Sends the
// playback request, polls for completion or timeout, and tears the query
// down. The PlaybackQuery* must already be installed in activeQueryMap.
int chl::ClientQueryService::dispatch_query(chl::PlaybackQuery* query, PlaybackQueryRpcClient* playbackRpcClient)
{
    if((playbackRpcClient == nullptr) ||
       (playbackRpcClient->send_story_playback_request(query->queryId,
                                                       query->storyId,
                                                       query->chronicleName,
                                                       query->storyName,
                                                       query->startTime,
                                                       query->endTime) != chl::CL_SUCCESS))
    {
        stop_query(query->queryId);
        return chl::CL_ERR_NO_PLAYERS;
    }

    // Poll for completion. `query->timeout_time` is set once at construction and
    // never mutated, so reading it lock-free is safe; `query->completed` is written
    // by receive_query_response() under queryServiceMutex, so it must be read under
    // the same lock. The query object itself stays alive throughout: only this
    // thread ever erases it from activeQueryMap (below or via stop_query()), so the
    // raw pointer remains valid for the duration of the call.
    uint64_t current_time = std::chrono::steady_clock::now().time_since_epoch().count();
    bool completed = false;
    while(current_time < query->timeout_time)
    {
        {
            std::lock_guard<std::mutex> lock(queryServiceMutex);
            if(query->completed)
            {
                completed = true;
                break;
            }
        }
        sleep(1); //TODO: replace with finer granularity sleep...
        current_time = std::chrono::steady_clock::now().time_since_epoch().count();
    }

    // Decide the outcome and remove the query from the active map in a single
    // critical section, so a response that is still arriving cannot race with the
    // teardown. Everything the caller needs is extracted before the erase (which
    // destroys the PlaybackQuery). We deliver to the caller-owned vector/callback
    // *after* releasing the lock, and only while dispatch_query() is still on the
    // stack, which guarantees that caller-owned state is still alive.
    std::vector<chl::Event> delivered_events;
    std::vector<chl::Event>* eventSeries = nullptr;
    chl::Client::EventCallback callback;
    {
        std::lock_guard<std::mutex> lock(queryServiceMutex);
        // Re-check under the lock: the response may have landed between the last
        // poll read and here.
        completed = query->completed;
        if(completed)
        {
            delivered_events = std::move(query->resultEvents);
            eventSeries = query->eventSeries;
            callback = std::move(query->callback);
        }
        activeQueryMap.erase(query->queryId);
    }

    if(!completed)
    {
        return chl::CL_ERR_QUERY_TIMED_OUT;
    }

    if(eventSeries != nullptr)
    {
        *eventSeries = std::move(delivered_events);
    }
    else if(callback)
    {
        for(auto const& event: delivered_events)
        {
            // Per the public-header contract the callback must not throw; we still
            // guard against it so a misbehaving callback can't abort the client or
            // break subsequent queries.
            try
            {
                callback(event);
            }
            catch(std::exception const& ex)
            {
                LOG_ERROR("[ClientQueryService] User ReplayStory callback threw '{}'; "
                          "skipping event and continuing",
                          ex.what());
            }
            catch(...)
            {
                LOG_ERROR("[ClientQueryService] User ReplayStory callback threw non-std exception; "
                          "skipping event and continuing");
            }
        }
    }

    return chl::CL_SUCCESS;
}

int chl::ClientQueryService::replay_story(chl::ChronicleName const& chronicle,
                                          chl::StoryName const& story,
                                          uint64_t start,
                                          uint64_t end,
                                          std::vector<chl::Event>& event_series)
{
    chl::StoryId story_id;
    chl::PlaybackQueryRpcClient* playbackRpcClient = nullptr;
    {
        // acquiredStoryMap is mutated by addStoryReader()/removeStoryReader() under
        // queryServiceMutex; look it up under the same lock and copy out what we need.
        std::lock_guard<std::mutex> lock(queryServiceMutex);
        auto storyReader_iter = acquiredStoryMap.find(std::pair<chl::ChronicleName, chl::StoryName>(chronicle, story));
        if(storyReader_iter == acquiredStoryMap.end())
        {
            return chl::CL_ERR_NOT_ACQUIRED;
        }
        story_id = (*storyReader_iter).second.first;
        playbackRpcClient = (*storyReader_iter).second.second;
    }

    auto timeout_time =
            (std::chrono::steady_clock::now() + std::chrono::seconds(queryTimeoutInSecs)).time_since_epoch().count();

    chl::PlaybackQuery* query = start_query(timeout_time, story_id, chronicle, story, start, end, event_series);
    if(query == nullptr)
    {
        return chl::CL_ERR_UNKNOWN;
    }
    return dispatch_query(query, playbackRpcClient);
}

int chl::ClientQueryService::replay_story(chl::ChronicleName const& chronicle,
                                          chl::StoryName const& story,
                                          uint64_t start,
                                          uint64_t end,
                                          chl::Client::EventCallback callback)
{
    chl::StoryId story_id;
    chl::PlaybackQueryRpcClient* playbackRpcClient = nullptr;
    {
        // acquiredStoryMap is mutated by addStoryReader()/removeStoryReader() under
        // queryServiceMutex; look it up under the same lock and copy out what we need.
        std::lock_guard<std::mutex> lock(queryServiceMutex);
        auto storyReader_iter = acquiredStoryMap.find(std::pair<chl::ChronicleName, chl::StoryName>(chronicle, story));
        if(storyReader_iter == acquiredStoryMap.end())
        {
            return chl::CL_ERR_NOT_ACQUIRED;
        }
        story_id = (*storyReader_iter).second.first;
        playbackRpcClient = (*storyReader_iter).second.second;
    }

    auto timeout_time =
            (std::chrono::steady_clock::now() + std::chrono::seconds(queryTimeoutInSecs)).time_since_epoch().count();

    chl::PlaybackQuery* query = start_query(timeout_time, story_id, chronicle, story, start, end, std::move(callback));
    if(query == nullptr)
    {
        return chl::CL_ERR_UNKNOWN;
    }
    return dispatch_query(query, playbackRpcClient);
}
//////

chl::PlaybackQuery* chl::ClientQueryService::start_query(uint64_t timeout_time,
                                                         chl::StoryId const& story_id,
                                                         chl::ChronicleName const& chronicle,
                                                         chl::StoryName const& story,
                                                         chl::chrono_time const& start_time,
                                                         chl::chrono_time const& end_time,
                                                         std::vector<chl::Event>& playback_events)
{
    std::lock_guard<std::mutex> lock(queryServiceMutex);

    uint32_t query_id = ++queryIndex;

    auto insert_return =
            activeQueryMap.insert(std::pair<uint32_t, chl::PlaybackQuery>(query_id,
                                                                          chl::PlaybackQuery(playback_events,
                                                                                             query_id,
                                                                                             timeout_time,
                                                                                             story_id,
                                                                                             chronicle,
                                                                                             story,
                                                                                             start_time,
                                                                                             end_time)));

    if(insert_return.second)
    {
        LOG_DEBUG("[ClientQueryService] started query {} for story {} {}-{} time range {}-{}",
                  query_id,
                  story_id,
                  chronicle,
                  story,
                  start_time,
                  end_time);
        return &(*insert_return.first).second;
    }
    else
    {
        return nullptr;
    }
}

chl::PlaybackQuery* chl::ClientQueryService::start_query(uint64_t timeout_time,
                                                         chl::StoryId const& story_id,
                                                         chl::ChronicleName const& chronicle,
                                                         chl::StoryName const& story,
                                                         chl::chrono_time const& start_time,
                                                         chl::chrono_time const& end_time,
                                                         chl::Client::EventCallback callback)
{
    std::lock_guard<std::mutex> lock(queryServiceMutex);

    uint32_t query_id = ++queryIndex;

    auto insert_return =
            activeQueryMap.insert(std::pair<uint32_t, chl::PlaybackQuery>(query_id,
                                                                          chl::PlaybackQuery(std::move(callback),
                                                                                             query_id,
                                                                                             timeout_time,
                                                                                             story_id,
                                                                                             chronicle,
                                                                                             story,
                                                                                             start_time,
                                                                                             end_time)));

    if(insert_return.second)
    {
        LOG_DEBUG("[ClientQueryService] started query {} for story {} {}-{} time range {}-{}",
                  query_id,
                  story_id,
                  chronicle,
                  story,
                  start_time,
                  end_time);
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
// directly to the active query's event series.
void chl::ClientQueryService::receive_query_response(tl::request const& request, tl::bulk& b)
{
    try
    {
        tl::endpoint ep = request.get_endpoint();
        LOG_DEBUG("[ClientQueryService] receive_query_response :Endpoint obtained, ThreadID={}", tl::thread::self_id());

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

        chronolog::PlaybackQueryResponse response(0);
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

        LOG_DEBUG("[ClientQueryService] PlaybackQueryResponse received: query_id {} eventCount {} ThreadID={}",
                  response.query_id,
                  response.events.size(),
                  tl::thread::self_id());

        // Stash the arriving events into the query's own buffer under the lock and
        // mark it completed. We deliberately do NOT touch query.eventSeries or invoke
        // query.callback here: those reference caller-owned state that a timed-out
        // caller may already have destroyed, and this handler runs on a Thallium ULT
        // that races the polling thread's teardown. dispatch_query() takes the same
        // lock to check `completed` and erase the query, so a response either lands
        // fully before the erase or finds the entry already gone. Delivery to the
        // caller happens on the polling thread, while the caller is still blocked in
        // dispatch_query() (see issue #690).
        {
            std::lock_guard<std::mutex> lock(queryServiceMutex);
            auto query_iter = activeQueryMap.find(response.query_id);
            if(query_iter != activeQueryMap.end())
            {
                chl::PlaybackQuery& query = (*query_iter).second;
                query.resultEvents = std::move(response.events);
                query.completed = true;
                LOG_DEBUG("[ClientQueryService] Query {} got {} events, ThreadID={}",
                          response.query_id,
                          query.resultEvents.size(),
                          tl::thread::self_id());
            }
            else
            {
                LOG_DEBUG("[ClientQueryService] Query {} no longer active (timed out or released); "
                          "discarding response, ThreadID={}",
                          response.query_id,
                          tl::thread::self_id());
            }
        }

        LOG_TRACE("[ClientQueryService] PlaybackQueryResponse recording RPC response {}, ThreadID={}",
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
