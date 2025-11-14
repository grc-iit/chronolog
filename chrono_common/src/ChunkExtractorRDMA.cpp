#include <thallium/serialization/stl/vector.hpp>
#include <cereal/archives/binary.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <RDMATransferAgent.h>
#include <ChunkExtractorRDMA.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::StoryChunkExtractorRDMA::StoryChunkExtractorRDMA(tl::engine &tl_engine, chronolog::ServiceId const& receiving_service_id)
        : sender_tl_engine(tl_engine)
        , receiver_service_id(receiving_service_id) 
        , rdma_sender(nullptr)
{
    try
    {
        rdma_sender = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, receiver_service_id);
        LOG_TRACE("[ChunkExtractorRDMA] Constructor created rdma_sender for receiver_service {} ", chl::to_string(receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[ChunkExtractorRDMA] Constructor : failed to create rdma_sender for receiver_service {} ", chl::to_string(receiver_service_id));
        rdma_sender = nullptr;
    }

}

chronolog::StoryChunkExtractorRDMA::StoryChunkExtractorRDMA( StoryChunkExtractorRDMA const& other)
        : sender_tl_engine(other.get_sender_engine())
        , receiver_service_id(other.get_receiver_service_id()) 
        , rdma_sender(nullptr)
{
    try
    {
        rdma_sender = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, receiver_service_id);
        LOG_TRACE("[ChunkExtractorRDMA] Constructor copy: created rdma_sender for receiver_service {} ", chl::to_string(receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[ChunkExtractorRDMA] Constructor: failed to create rdma_sender for receiver_service {} ", chl::to_string(receiver_service_id));
        rdma_sender = nullptr;
    }
    
}

chl::StoryChunkExtractorRDMA & chronolog::StoryChunkExtractorRDMA::operator=( StoryChunkExtractorRDMA const& other)
{
    if(this != &other)
    {
        if(rdma_sender != nullptr)
        {
            LOG_TRACE("[ChunkExtractorRDMA] assingment : deleting receiver_service {} ", chl::to_string(receiver_service_id));

            delete rdma_sender;
        }

        sender_tl_engine = other.get_sender_engine();
        receiver_service_id = other.get_receiver_service_id(); 
    
        try
        {
            rdma_sender = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, receiver_service_id);
            LOG_TRACE("[ChunkExtractorRDMA] assingment: created rdma_sender for receiver_service {} ", chl::to_string(receiver_service_id));
        }
        catch(...)
        {
            LOG_ERROR("[ChunkExtractorRDMA] assignment: failed to create rdma_sender for receiver_service {} ", chl::to_string(receiver_service_id));
            rdma_sender = nullptr;
        }  
    }

    return *this;
}
    

chronolog::StoryChunkExtractorRDMA::~StoryChunkExtractorRDMA()
{
    if(rdma_sender != nullptr)
    {
        LOG_TRACE("[ChunkExtractorRDMA] Destructor: deleting receiver_service {} ", chl::to_string(receiver_service_id));
        delete rdma_sender;
    }
}

////

int chronolog::StoryChunkExtractorRDMA::process_chunk(chronolog::StoryChunk * story_chunk)
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

        auto transfer_return = rdma_sender->transfer_serialized_story_chunk(serialized_story_chunk);

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
