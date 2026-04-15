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
        LOG_TRACE("[DualEndpointChunkExtractor] Constructor created rdma_sender for player_receiver {} ", chl::to_string(player_receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[DualEndpointChunkExtractor] Constructor : failed to create rdma_sender for player_receiver {} ", chl::to_string(player_receiver_service_id));
        rdma_sender_for_player = nullptr;
    }

    try
    {
        rdma_sender_for_grapher = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, grapher_receiver_service_id);
        LOG_TRACE("[DualEndpointChunkExtractor] Constructor created rdma_sender for grpaher_receiver {} ", chl::to_string(grapher_receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[DualEndpointChunkExtractor] Constructor : failed to create rdma_sender for grapher_receiver {} ", chl::to_string(grapher_receiver_service_id));
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
            LOG_ERROR("[DualEndpointExtractorRDMA] assignment: failed to create rdma_sender for receiver {} ", chl::to_string(grapher_receiver_service_id));
            rdma_sender_for_grapher = nullptr;
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
  
    // Note: a failure to transfer a story_chunk to the ChronoPlayer alone is not considered a critical enough error 
    // to exit this function and try again later. In this case the error would be logged but the function would 
    // proceed to sending the story_chunk to the ChronoGrapher
    // Failure to transfer the story_chunk to the ChronoGrapher would trigger the exit from the function,
    // return the StoryChunk back on the extraction_queue, and subsequent retry

    int transfer_return = chl::CL_ERR_UNKNOWN;

    try
    {
        LOG_DEBUG("[DualEndpointChunkExtractor] tl::thread_id={} processing chunk StoryId={} {}-{} {}-{} eventCount {}",         
                thallium::thread::self_id(), story_chunk->getStoryId(),
                story_chunk->getChronicleName(),story_chunk->getStoryName(),story_chunk->getStartTime(), story_chunk->getEndTime(),story_chunk->getEventCount());

        if(rdma_sender_for_grapher == nullptr) 
        { 
            LOG_ERROR("[DualEndpointChunkExtractor] Failed to transfer StoryChunk StoryId={} StartTime={}", story_chunk->getStoryId(), story_chunk->getStartTime());
            return chl::CL_ERR_STORY_CHUNK_EXTRACTION; 
        }

        std::ostringstream oss(std::ios::binary);
        try
        {
            cereal::BinaryOutputArchive oarchive(oss);
            oarchive(*story_chunk);
        }
        catch(cereal::Exception const &ex)
        {
            LOG_ERROR("[DualEndpointChunkExtractor] Cereal exception while serializing StoryChunk StoryId={} StartTime={} ex {}", story_chunk->getStoryId(), story_chunk->getStartTime() ,ex.what());
        }


        std::string serialized_story_chunk = oss.str();

        if(rdma_sender_for_player != nullptr)
        {
            try
            {
                transfer_return = rdma_sender_for_player->transfer_serialized_story_chunk(serialized_story_chunk);
                if(transfer_return == chl::CL_SUCCESS)
                {
                    LOG_INFO("[DualEndpointChunkExtractor] Transfered to Player StoryChunk StoryId={} StartTime={}", story_chunk->getStoryId(), story_chunk->getStartTime());
                }
                else
                {
                    LOG_ERROR("[DualEndpointChunkExtractor] Failed to transfer to Player StoryChunk StoryId={} StartTime={}", story_chunk->getStoryId(), story_chunk->getStartTime());
                }
            }
            catch(std::exception const &ex)
            {
                LOG_ERROR("[DualEndpointChunkExtractor] Standard exception while serializing StoryChunk StoryId={} StartTime={} ex {}", 
                     story_chunk->getStoryId(), story_chunk->getStartTime() ,ex.what());
            }
        }

        transfer_return = rdma_sender_for_grapher->transfer_serialized_story_chunk(serialized_story_chunk);

        if(transfer_return == chl::CL_SUCCESS)
        {
            LOG_INFO("[DualEndpointChunkExtractor] Transfered to Grapher StoryChunk StoryId={} StartTime={}", story_chunk->getStoryId(), story_chunk->getStartTime());
        }
        else
        {
            LOG_ERROR("[DualEndpointChunkExtractor] Failed to transfer to Grapher StoryChunk StoryId={} StartTime={}", story_chunk->getStoryId(), story_chunk->getStartTime());
        }
        
        return transfer_return;
    }
    catch(std::exception const &ex)
    {
        LOG_ERROR("[DualEndpointChunkExtractor] Standard exception while serializing StoryChunk StoryId={} StartTime={} ex {}", story_chunk->getStoryId(), story_chunk->getStartTime() ,ex.what());
    }
    catch(...)
    {
        LOG_ERROR("[DualEndpointChunkExtractor] Exception while transferring StoryChunkiStoryId={} StartTime={}", story_chunk->getStoryId(), story_chunk->getStartTime());
    }

return chl::CL_ERR_STORY_CHUNK_EXTRACTION;
}
