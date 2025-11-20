#include <thallium/serialization/stl/vector.hpp>
#include <cereal/archives/binary.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <RDMATransferAgent.h>
#include <DualEndpointChunkExtractorRDMA.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::DualEndpointChunkExtractorRDMA::DualEndpointChunkExtractorRDMA(tl::engine &tl_engine, chronolog::ServiceId const& player_receiving_service_id, chronolog::ServiceId const& grapher_receiving_service_id)
        : sender_tl_engine(tl_engine)
        , player_receiver_service_id(player_receiving_service_id) 
        , grapher_receiver_service_id(grapher_receiving_service_id) 
        , rdma_sender_for_player(nullptr)
        , rdma_sender_for_grapher(nullptr)
{
    try
    {
        rdma_sender_for_player = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, player_receiver_service_id);
        LOG_TRACE("[ChunkExtractorRDMA] Constructor created rdma_sender for player_receiver {} ", chl::to_string(player_receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[ChunkExtractorRDMA] Constructor : failed to create rdma_sender for player_receiver {} ", chl::to_string(player_receiver_service_id));
        rdma_sender_for_player = nullptr;
    }

    try
    {
        rdma_sender_for_grapher = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, grapher_receiver_service_id);
        LOG_TRACE("[ChunkExtractorRDMA] Constructor created rdma_sender for grpaher_receiver {} ", chl::to_string(grapher_receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[ChunkExtractorRDMA] Constructor : failed to create rdma_sender for grapher_receiver {} ", chl::to_string(grapher_receiver_service_id));
        rdma_sender_for_grapher = nullptr;
    }

}

chronolog::DualEndpointChunkExtractorRDMA::DualEndpointChunkExtractorRDMA( DualEndpointChunkExtractorRDMA const& other)
        : sender_tl_engine(other.get_sender_engine())
        , player_receiver_service_id(other.get_player_receiver_id()) 
        , grapher_receiver_service_id(other.get_grapher_receiver_id()) 
        , rdma_sender_for_player(nullptr)
        , rdma_sender_for_grapher(nullptr)
{
    try
    {
        rdma_sender_for_player = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, player_receiver_service_id);
        LOG_TRACE("[DualEndpointExtractorRDMA] Copy Constructor created rdma_sender for player_receiver {} ", chl::to_string(player_receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[DualEndpointExtractorRDMA] Constructor : failed to create rdma_sender for player_receiver {} ", chl::to_string(player_receiver_service_id));
        rdma_sender_for_player = nullptr;
    }

    try
    {
        rdma_sender_for_grapher = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, grapher_receiver_service_id);
        LOG_TRACE("Dual[DualEndpointExtractorRDMA] Copy Constructor created rdma_sender for grpaher_receiver {} ", chl::to_string(grapher_receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[DualEndpointExtractorRDMA] Constructor : failed to create rdma_sender for grapher_receiver {} ", chl::to_string(grapher_receiver_service_id));
        rdma_sender_for_grapher = nullptr;
    }
}

chl::DualEndpointChunkExtractorRDMA & chronolog::DualEndpointChunkExtractorRDMA::operator=( DualEndpointChunkExtractorRDMA const& other)
{
    if(this == &other)
    {
        return *this;
    }

    if(rdma_sender_for_player != nullptr)
    {
        LOG_TRACE("[DualEndpointExtractorRDMA] assingment : deleting receiver {} ", chl::to_string(player_receiver_service_id));
        delete rdma_sender_for_player;
    }
    if(rdma_sender_for_grapher != nullptr)
    {
        LOG_TRACE("[DualEndpointExtractorRDMA] assingment : deleting receiver {} ", chl::to_string(grapher_receiver_service_id));
        delete rdma_sender_for_grapher;
    }

    sender_tl_engine = other.get_sender_engine();
    player_receiver_service_id = other.get_player_receiver_id(); 
    grapher_receiver_service_id = other.get_player_receiver_id(); 
    
        try
        {
            rdma_sender_for_player = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, player_receiver_service_id);
            LOG_TRACE("[DualEndpointExtractorRDMA] assingment: created rdma_sender for receiver {} ", chl::to_string(player_receiver_service_id));
        }
        catch(...)
        {
            LOG_ERROR("[DualEndpointExtractorRDMA] assignment: failed to create rdma_sender for receiver {} ", chl::to_string(player_receiver_service_id));
            rdma_sender_for_player = nullptr;
        }
  
        try
        {
            rdma_sender_for_grapher = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, grapher_receiver_service_id);
            LOG_TRACE("[DualEndpointExtractorRDMA] assingment: created rdma_sender for receiver {} ", chl::to_string(grapher_receiver_service_id));
        }
        catch(...)
        {
            LOG_ERROR("[DualEndpointExtractorRDMA] assignment: failed to create rdma_sender for receiver {} ", chl::to_string(grpaher_receiver_service_id));
            rdma_sender_for_grapher = nullptr;
        }  
    }

    return *this;
}
    

chronolog::DualEndpointChunkExtractorRDMA::~DualEndpointChunkExtractorRDMA()
{
    if(rdma_sender_for_player != nullptr)
    {
        LOG_TRACE("[DualEndpointExtractorRDMA] Destructor: deleting receiver {} ", chl::to_string(player_receiver_service_id));
        delete rdma_sender_for_player;
    }
    if(rdma_sender_for_grapher != nullptr)
    {
        LOG_TRACE("[DualEndpointExtractorRDMA] Destructor: deleting receiver {} ", chl::to_string(grapher_receiver_service_id));
        delete rdma_sender_for_grapher;
    }
}

////

int chronolog::DualEndpointChunkExtractorRDMA::process_chunk(chronolog::StoryChunk * story_chunk)
{
    try
    {
        LOG_DEBUG("[ExtractorRDMA] tl::thread_id={} processing chunk StoryId={} {}-{} {}-{} eventCount {}",         
                thallium::thread::self_id(), story_chunk->getStoryId(),
                story_chunk->getChronicleName(),story_chunk->getStoryName(),story_chunk->getStartTime(), story_chunk->getEndTime(),story_chunk->getEventCount());

        if(rdma_sender == nullptr) 
        { 
            LOG_ERROR("[ChunkExtractorRDMA] Failed to transfer StoryChunk StoryId={} StartTime={}", story_chunk->getStoryId(), story_chunk->getStartTime());
            return chl::CL_ERR_UNKNOWN; 
        }

        std::ostringstream oss(std::ios::binary);
        cereal::BinaryOutputArchive oarchive(oss);
        oarchive(*story_chunk);
        std::string serialized_story_chunk = oss.str();

        auto transfer_return = rdma_sender_for_grapher->transfer_serialized_bulk(serialized_story_chunk);

        if(transfer_return == chl::CL_SUCCESS)
        {
            LOG_INFO("[ChunkExtractorRDMA] Transfered StoryChunk StoryId={} StartTime={}", story_chunk->getStoryId(), story_chunk->getStartTime());
        }
        else
        {
            LOG_ERROR("[ChunkExtractorRDMA] Failed to transfer StoryChunk StoryId={} StartTime={}", story_chunk->getStoryId(), story_chunk->getStartTime());
        }
        
        return transfer_return;
    }
    catch(cereal::Exception const &ex)
    {
        LOG_ERROR("[RDMATransferAgent] Cereal exception while serializing StoryChunk StoryId={} StartTime={} ex {}", story_chunk->getStoryId(), story_chunk->getStartTime() ,ex.what());
    }
    catch(std::exception const &ex)
    {
        LOG_ERROR("[RDMATransferAgent] Standard exception while serializing StoryChunk StoryId={} StartTime={} ex {}", story_chunk->getStoryId(), story_chunk->getStartTime() ,ex.what());
    }
    catch(...)
    {
        LOG_ERROR("[ChunkExtractorRDMA] Exception while  transferring StoryChunkiStoryId={} StartTime={}", story_chunk->getStoryId(), story_chunk->getStartTime());
    }

return chl::CL_ERR_STORY_CHUNK_EXTRACTION;
}
