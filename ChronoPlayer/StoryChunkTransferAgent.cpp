#include <thallium/serialization/stl/vector.hpp>
#include <cereal/archives/binary.hpp>

#include "chrono_monitor.h"
#include "chronolog_errcode.h"
#include "StoryChunkTransferAgent.h"

namespace tl = thallium;
namespace chl = chronolog;

chronolog::StoryChunkTransferAgent::StoryChunkTransferAgent(tl::engine &tl_engine, chronolog::ServiceId const& service_id)
        : service_engine(tl_engine) 
        , receiver_service_id(service_id)
{
    std::string service_addr_string;
    receiver_service_id.get_service_as_string(service_addr_string);
    
    LOG_DEBUG("[StoryChunkTransferAgent] Constructor for receiver service {} , service_string {}", chl::to_string(receiver_service_id), service_addr_string);
    receiver_service_handle = tl::provider_handle(service_engine.lookup(service_addr_string), receiver_service_id.getProviderId());

    receiver_is_available = service_engine.define("receiver_is_available");
    receive_story_chunk = service_engine.define("receive_story_chunk");

    LOG_DEBUG("[StoryChunkTransferAgent] created agent for receiver service {}", chl::to_string(receiver_service_id));
}

chronolog::StoryChunkTransferAgent::~StoryChunkTransferAgent()
{
    receiver_is_available.deregister();
    receive_story_chunk.deregister();
    LOG_DEBUG("[StoryChunkTransferAgent] Destroying agent for receiver service {}", chl::to_string(receiver_service_id));
}

bool chronolog::StoryChunkTransferAgent::is_receiver_available() const
{
    bool ret_value = receiver_is_available.on(receiver_service_handle)( );

    LOG_DEBUG("[StoryChunkTransferAgent] receiver_service {} is available {}", chl::to_string(receiver_service_id), ret_value);
return ret_value;    
}
int chronolog::StoryChunkTransferAgent::processStoryChunk(chronolog::StoryChunk*story_chunk)
{
    try
    {
        LOG_DEBUG("[StoryChunkTransferAgent] agent for receiver {} processing a story chunk, StoryID: {}, StartTime: {}", chl::to_string(receiver_service_id)
                  , story_chunk->getStoryId(), story_chunk->getStartTime());
#ifdef LOGTIME
        std::chrono::high_resolution_clock::time_point start, end;
        start = std::chrono::high_resolution_clock::now();
#endif
        size_t serialized_story_chunk_size;
        std::ostringstream oss(std::ios::binary);
        cereal::BinaryOutputArchive oarchive(oss);
        oarchive(*story_chunk);
        std::string serialized_story_chunk = oss.str();
        serialized_story_chunk_size = serialized_story_chunk.size();

#ifdef LOGTIME
        end = std::chrono::high_resolution_clock::now();
        LOG_INFO("[StoryChunkTransferAgent] StoryChunk serialization took {} us",
                std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count() / 1000.0);
#endif
        LOG_DEBUG("[StoryChunkTransferAgent] Serialized StoryChunk size: {}", serialized_story_chunk_size);

        std::vector <std::pair <void*, std::size_t>> segments(1);
        segments[0].first = (void*)(serialized_story_chunk.data());
        segments[0].second = serialized_story_chunk_size;
        tl::bulk tl_bulk = service_engine.expose(segments, tl::bulk_mode::read_only);
        LOG_DEBUG("[StoryChunkTransferAgent] Draining StoryChunk size: {} ...", tl_bulk.size());

        size_t bytes_transfered = receive_story_chunk.on(receiver_service_handle)(tl_bulk);

#ifdef LOGTIME 
        start = end;
        end = std::chrono::high_resolution_clock::now();
        LOG_INFO("[StoryChunkTransferAgent] StoryChunk transfer took {} us",
                std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count() / 1000.0);
#endif
        LOG_DEBUG("[StoryChunkTransferAgent] StoryChunk transfer returned with result: {}", bytes_transfered);

        if(bytes_transfered == serialized_story_chunk_size)
        {
            LOG_INFO("[StoryChunkTransferAgent] Successfully transfered StoryChunk, StoryId:{}, StartTime:{}", story_chunk->getStoryId(), story_chunk->getStartTime());
            return chronolog::CL_SUCCESS;
        }
    }
    catch(tl::exception const &ex)
    {
        LOG_ERROR("[StoryChunkTransferAgent] Thallium exception while transferring StoryChunk: {}", ex.what());
        return (chronolog::CL_ERR_UNKNOWN);
    }
    catch(cereal::Exception const &ex)
    {
        LOG_ERROR("[StoryChunkTransferAgent] Cereal exception while serializing StoryChunk: {}", ex.what());
        return chronolog::CL_ERR_UNKNOWN;
    }
    catch(std::exception const &ex)
    {
        LOG_ERROR("[StoryChunkTransferAgent] Standard exception while serializing StoryChunk: {}", ex.what());
        return chronolog::CL_ERR_UNKNOWN;
    }
    catch(...)
    {
        LOG_ERROR("[StoryChunkTransferAgent] Unknown exception while  transferring StoryChunk.");
        return chronolog::CL_ERR_UNKNOWN;
    }

    LOG_ERROR("[StoryChunkTransferAgent] Failed to transfer StoryShunk, StoryId:{},StartTime:{}", story_chunk->getStoryId(), story_chunk->getStartTime());
    return chronolog::CL_ERR_STORY_CHUNK_EXTRACTION;
}

