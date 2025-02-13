

#include <thallium.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <cereal/archives/binary.hpp>

#include "chronolog_errcode.h"
#include "chrono_monitor.h"
#include "StoryChunk.h"
#include "ClientQueryService.h"
#include "PlaybackQueryRpcClient.h"

namespace tl = thallium;
namespace chl = chronolog;



chl::ClientQueryService::ClientQueryService(thallium::engine & tl_engine, chl::ServiceId const& client_service_id)
        : tl::provider <ClientQueryService>(tl_engine, client_service_id.getProviderId())
        , queryServiceEngine(tl_engine)
        , queryServiceId(client_service_id)
{

    LOG_DEBUG("[ClientQueryService] created  service {}", chl::to_string(queryServiceId));

         define("receive_story_chunk", &ClientQueryService::receive_story_chunk, tl::ignore_return_value());
         //set up callback for the case when the engine is being finalized while this provider is still alive
         get_engine().push_finalize_callback(this, [p = this]()
         { delete p; });
}


chl::ClientQueryService::~ClientQueryService()
{
    LOG_DEBUG("[ClientQueryService] Destructor called. Cleaning up...");
    get_engine().pop_finalize_callback(this);
}


// record the newly issued query details and return queryId
uint32_t chl::ClientQueryService::start_new_query(chl::ChronicleName const& chronicle, chl::StoryName const& story, 
        chl::chrono_time const& start_time, chl::chrono_time const& end_time)
{
    std::lock_guard <std::mutex> lock(queryServiceMutex);

    uint32_t query_id = queryIdIndex++; 

    activeQueryMap.insert(std::pair<uint32_t, chl::StoryPlaybackQuery>
                ( query_id, chl::StoryPlaybackQuery( query_id, chronicle, story, start_time, end_time)));

    return query_id;
}

// find or create PlaybackServiceRpcClient associated with the remote Playback Service
chl::PlaybackQueryRpcClient * chronolog::ClientQueryService::addPlaybackQueryClient(chl::ServiceId const& player_card)
{
    LOG_DEBUG("[ClientQueryService] adding PlaybackQueryRpcClient for {}", chl::to_string(player_card));


    auto find_iter = playbackRpcClientMap.find(player_card.get_service_endpoint());

    if(find_iter != playbackRpcClientMap.end())
    {
        LOG_DEBUG("[ClientQueryService] found PlaybackQueryRpcClient for {}", chl::to_string(player_card));
        return (*find_iter).second;
    }

    chl::PlaybackQueryRpcClient * playbackRpcClient = nullptr;
    LOG_DEBUG("[ClientQueryService] adding PlaybackQueryRpcClient for {}", chl::to_string(player_card));
    
    std::lock_guard <std::mutex> lock(queryServiceMutex);

    try
    {
        playbackRpcClient = chronolog::PlaybackQueryRpcClient::CreatePlaybackQueryRpcClient(*this, player_card); 

        auto insert_return = playbackRpcClientMap.insert(
                std::pair <chl::service_endpoint, chl::PlaybackQueryRpcClient*>(
                        player_card.get_service_endpoint(), playbackRpcClient));

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
    catch(tl::exception const &ex)
    {
        playbackRpcClient = nullptr;
        LOG_DEBUG("[ClientQueryService] Failed to create PlaybackQueryRpcClient}", chl::to_string(player_card));
    }
    return playbackRpcClient;
}

// we are notified of the remote PlaybackService stopping, remove associated PlaybackQueryRpcClient
void chronolog::ClientQueryService::removePlaybackQueryClient(chl::ServiceId const& player_card)
{
    std::lock_guard <std::mutex> lock(queryServiceMutex);
    //TODO : we need to check that there no active queries associated with this PlaybackQueryRpcClient
    // to safely remove it
}

// build transfer of the Response StoryChunks
void chl::ClientQueryService::receive_story_chunk(tl::request  const& request, tl::bulk &b)
{
    try
    {
        tl::endpoint ep = request.get_endpoint();
        LOG_DEBUG("[ClientQueryService] receive_story_chunk :Endpoint obtained, ThreadID={}", tl::thread::self_id());
        
        std::vector <char> mem_vec(b.size());
        std::vector <std::pair <void*, std::size_t>> segments(1);
        segments[0].first = (void*)(&mem_vec[0]);
        segments[0].second = mem_vec.size();
        LOG_DEBUG("[ClientQueryService] Bulk memory prepared, size: {}, ThreadID={}", mem_vec.size(), tl::thread::self_id());
        tl::engine local_engine = get_engine();
        
        tl::bulk local = local_engine.expose(segments, tl::bulk_mode::write_only);
        LOG_DEBUG("[ClientQueryService] Bulk memory exposed, ThreadID={}", tl::thread::self_id());
        b.on(ep) >> local;
        LOG_DEBUG("[ClientQueryService] Received {} bytes of StoryChunk data, ThreadID={}", b.size(), tl::thread::self_id());
  
        StoryChunk*story_chunk = new StoryChunk();
        int ret = deserializedWithCereal(&mem_vec[0], b.size()
                                               , *story_chunk);
        if(ret != CL_SUCCESS)
        {
            LOG_ERROR("[ClientQueryService] Failed to deserialize a story chunk, ThreadID={}"
                            , tl::thread::self_id());
            delete story_chunk;
            ret = 10000000 + tl::thread::self_id(); // arbitrary error code encoded with thread id
            LOG_ERROR("[ClientQueryService] Discarding the story chunk, responding {} to Keeper", ret);
            request.respond(ret);
            return;
        }
        LOG_DEBUG("[ClientQueryService] StoryChunk received: StoryId {} StartTime {} eventCount {} ThreadID={}"
                        , story_chunk->getStoryId(), story_chunk->getStartTime(), story_chunk->getEventCount()
                        , tl::thread::self_id());
  
        request.respond(b.size());
        LOG_DEBUG("[ClientQueryService] StoryChunk recording RPC responded {}, ThreadID={}", b.size()
                        , tl::thread::self_id());
 
        // add StoryChunk to the QueryResponse Object 
        }
        catch(std::bad_alloc const &ex)
        {
            LOG_ERROR("[ClientQueryService] Failed to allocate memory for StoryChunk data, ThreadID={}" , tl::thread::self_id());
            request.respond(20000000 + tl::thread::self_id());
        }
}

int chl::ClientQueryService::deserializedWithCereal(char *buffer, size_t size, chl::StoryChunk &story_chunk)
     {
         std::stringstream ss(std::ios::binary | std::ios::in | std::ios::out);
         try
         {
             ss.write(buffer, size);
             cereal::BinaryInputArchive iarchive(ss);
             iarchive(story_chunk);
             return chl::CL_SUCCESS;
         }
         catch(cereal::Exception const &ex)
        {
            LOG_ERROR("[ClientQueryService] Failed to deserialize a story chunk, size={}, ThreadID={}. "
                       "Cereal exception: {}", ss.str().size(), tl::thread::self_id(), ex.what());
         }
         catch(std::exception const &ex)
         {
          LOG_ERROR("[ClientQueryService] Failed to deserialize a story chunk, size={}, ThreadID={}. "
                       "std::exception : {}", ss.str().size(), tl::thread::self_id(), ex.what());
         }
         catch(...)
         {
             LOG_ERROR("[ClientQueryService] Failed to deserialize a story chunk, ThreadID={}. Unknown exception "
                       "encountered.", tl::thread::self_id());
         }
        return chl::CL_ERR_UNKNOWN;
     }

