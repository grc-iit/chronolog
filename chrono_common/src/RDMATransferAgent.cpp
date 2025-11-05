#include <thallium/serialization/stl/vector.hpp>
#include <cereal/archives/binary.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <RDMATransferAgent.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::RDMATransferAgent::RDMATransferAgent(tl::engine &tl_engine, chronolog::ServiceId const& service_id)
        : service_engine(tl_engine) 
        , receiver_service_id(service_id)
{
    std::string service_addr_string;
    receiver_service_id.get_service_as_string(service_addr_string);
    
    LOG_DEBUG("[RDMATransferAgent] Constructor for receiver service {} , service_string {}", chl::to_string(receiver_service_id), service_addr_string);
    receiver_service_handle = tl::provider_handle(service_engine.lookup(service_addr_string), receiver_service_id.getProviderId());

    receiver_is_available = service_engine.define("receiver_is_available");
    receive_story_chunk = service_engine.define("receive_story_chunk");

    LOG_DEBUG("[RDMATransferAgent] created agent for receiver service {}", chl::to_string(receiver_service_id));
}

chronolog::RDMATransferAgent::~RDMATransferAgent()
{
    receiver_is_available.deregister();
    receive_story_chunk.deregister();
    LOG_DEBUG("[RDMATransferAgent] Destroying agent for receiver service {}", chl::to_string(receiver_service_id));
}

bool chronolog::RDMATransferAgent::is_receiver_available() const
{
    bool ret_value = receiver_is_available.on(receiver_service_handle)( );

    LOG_DEBUG("[RDMATransferAgent] receiver_service {} is available {}", chl::to_string(receiver_service_id), ret_value);
return ret_value;    
}

/*int chronolog::RDMATransferAgent::transfer_story_chunk(chronolog::StoryChunk*story_chunk)
{
    try
    {
        LOG_DEBUG("[RDMATransferAgent] agent for receiver {} transfering story chunk, StoryID: {}, StartTime: {}, event_count {}", chl::to_string(receiver_service_id)
                  , story_chunk->getStoryId(), story_chunk->getStartTime(), story_chunk->getEventCount());

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
        LOG_INFO("[RDMATransferAgent] StoryChunk serialization took {} us",
                std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count() / 1000.0);
#endif
        LOG_DEBUG("[RDMATransferAgent] Serialized StoryChunk size: {}", serialized_story_chunk_size);

        std::vector <std::pair <void*, std::size_t>> segments(1);
        segments[0].first = (void*)(serialized_story_chunk.data());
        segments[0].second = serialized_story_chunk_size;
        tl::bulk tl_bulk = service_engine.expose(segments, tl::bulk_mode::read_only);
        LOG_DEBUG("[RDMATransferAgent] Draining StoryChunk size: {} ...", tl_bulk.size());

        size_t bytes_transfered = receive_story_chunk.on(receiver_service_handle)(tl_bulk);

#ifdef LOGTIME 
        start = end;
        end = std::chrono::high_resolution_clock::now();
        LOG_INFO("[RDMATransferAgent] StoryChunk transfer took {} us",
                std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count() / 1000.0);
#endif
        LOG_DEBUG("[RDMATransferAgent] StoryChunk transfer returned with result: {}", bytes_transfered);

        if(bytes_transfered == serialized_story_chunk_size)
        {
            LOG_INFO("[RDMATransferAgent] Successfully transfered StoryChunk, StoryId:{}, StartTime:{}", story_chunk->getStoryId(), story_chunk->getStartTime());
            return chronolog::CL_SUCCESS;
        }
    }
    catch(tl::exception const &ex)
    {
        LOG_ERROR("[RDMATransferAgent] Thallium exception while transferring StoryChunk: {}", ex.what());
        return (chronolog::CL_ERR_UNKNOWN);
    }
    catch(cereal::Exception const &ex)
    {
        LOG_ERROR("[RDMATransferAgent] Cereal exception while serializing StoryChunk: {}", ex.what());
        return chronolog::CL_ERR_UNKNOWN;
    }
    catch(std::exception const &ex)
    {
        LOG_ERROR("[RDMATransferAgent] Standard exception while serializing StoryChunk: {}", ex.what());
        return chronolog::CL_ERR_UNKNOWN;
    }
    catch(...)
    {
        LOG_ERROR("[RDMATransferAgent] Unknown exception while  transferring StoryChunk.");
        return chronolog::CL_ERR_UNKNOWN;
    }

    LOG_ERROR("[RDMATransferAgent] Failed to transfer StoryShunk, StoryId:{},StartTime:{}", story_chunk->getStoryId(), story_chunk->getStartTime());
    return chronolog::CL_ERR_STORY_CHUNK_EXTRACTION;
}
*/
///////////////////////////////////
int chronolog::RDMATransferAgent::transfer_serialized_bulk( std::string const& serialized_story_chunk)
{
    try
    {
        size_t serialized_story_chunk_size;
        //std::ostringstream oss(std::ios::binary);
        //cereal::BinaryOutputArchive oarchive(oss);
        //oarchive(*story_chunk);
       // std::string serialized_story_chunk = oss.str();
        serialized_story_chunk_size = serialized_story_chunk.size();

        //LOG_DEBUG("[RDMATransferAgent] Serialized StoryChunk size: {}", serialized_story_chunk_size);

        std::vector <std::pair <void*, std::size_t>> segments(1);
        segments[0].first = (void*)(serialized_story_chunk.data());
        segments[0].second = serialized_story_chunk_size;
        tl::bulk tl_bulk = service_engine.expose(segments, tl::bulk_mode::read_only);
        LOG_DEBUG("[RDMATransferAgent] transfering Chunk size: {}  tl_bulk size ...",  serialized_story_chunk_size, tl_bulk.size());

        size_t bytes_transfered = receive_story_chunk.on(receiver_service_handle)(tl_bulk);

        LOG_DEBUG("[RDMATransferAgent] sent tl_bulk size {} transfered {} bytes", tl_bulk.size(), bytes_transfered);

        if(bytes_transfered == serialized_story_chunk_size)
        {
           // LOG_INFO("[RDMATransferAgent] Successfully transfered StoryChunk, StoryId:{}, StartTime:{}", story_chunk->getStoryId(), story_chunk->getStartTime());
            return chronolog::CL_SUCCESS;
        }
    }
    catch(tl::exception const &ex)
    {
        LOG_ERROR("[RDMATransferAgent] Thallium exception while transferring StoryChunk: {}", ex.what());
        return (chronolog::CL_ERR_UNKNOWN);
    }
    catch(...)
    {
        LOG_ERROR("[RDMATransferAgent] Unknown exception while  transferring StoryChunk.");
        return chronolog::CL_ERR_UNKNOWN;
    }

    return chronolog::CL_ERR_STORY_CHUNK_EXTRACTION;
}
