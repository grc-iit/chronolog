#include <thallium/serialization/stl/vector.hpp>
#include <cereal/archives/binary.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <streambuf>

#include <chrono_monitor.h>
#include <chronolog_profile.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <StoryChunkWireFormat.h>
#include <RDMATransferAgent.h>
#include <ChunkExtractorRDMA.h>

namespace tl = thallium;
namespace chl = chronolog;

namespace
{
bool envEnabled(char const* name)
{
    char const* value = std::getenv(name);
    return value != nullptr && (*value == '1' || std::strcmp(value, "true") == 0 || std::strcmp(value, "on") == 0 ||
                               std::strcmp(value, "yes") == 0);
}

class StringOutputBuffer: public std::streambuf
{
public:
    explicit StringOutputBuffer(std::string& output)
        : outputString(output)
    {}

protected:
    std::streamsize xsputn(char const* s, std::streamsize n) override
    {
        outputString.append(s, static_cast<std::size_t>(n));
        return n;
    }

    int overflow(int ch) override
    {
        if(ch == traits_type::eof())
        {
            return traits_type::not_eof(ch);
        }
        outputString.push_back(static_cast<char>(ch));
        return ch;
    }

private:
    std::string& outputString;
};

std::size_t estimateSerializedStoryChunkBytes(chronolog::StoryChunk const& story_chunk)
{
    std::size_t payload_bytes = 0;
    for(auto const& event: story_chunk)
    {
        payload_bytes += event.second.logRecord.size();
    }
    return payload_bytes + static_cast<std::size_t>(story_chunk.getEventCount()) * 64U +
           story_chunk.getChronicleName().size() + story_chunk.getStoryName().size() + 256U;
}
}

chronolog::StoryChunkExtractorRDMA::StoryChunkExtractorRDMA(tl::engine& tl_engine,
                                                            chronolog::ServiceId const& receiving_service_id)
    : sender_tl_engine(tl_engine)
    , receiver_service_id(receiving_service_id)
    , rdma_sender(nullptr)
{
    try
    {
        rdma_sender = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, receiver_service_id);
        LOG_TRACE("[ChunkExtractorRDMA] Constructor created rdma_sender for receiver_service {} ",
                  chl::to_string(receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[ChunkExtractorRDMA] Constructor : failed to create rdma_sender for receiver_service {} ",
                  chl::to_string(receiver_service_id));
        rdma_sender = nullptr;
    }
}

chronolog::StoryChunkExtractorRDMA::StoryChunkExtractorRDMA(StoryChunkExtractorRDMA const& other)
    : sender_tl_engine(other.get_sender_engine())
    , receiver_service_id(other.get_receiver_service_id())
    , rdma_sender(nullptr)
{
    try
    {
        rdma_sender = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, receiver_service_id);
        LOG_TRACE("[ChunkExtractorRDMA] Constructor copy: created rdma_sender for receiver_service {} ",
                  chl::to_string(receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[ChunkExtractorRDMA] Constructor: failed to create rdma_sender for receiver_service {} ",
                  chl::to_string(receiver_service_id));
        rdma_sender = nullptr;
    }
}

chl::StoryChunkExtractorRDMA& chronolog::StoryChunkExtractorRDMA::operator=(StoryChunkExtractorRDMA const& other)
{
    if(this != &other)
    {
        if(rdma_sender != nullptr)
        {
            LOG_TRACE("[ChunkExtractorRDMA] assingment : deleting receiver_service {} ",
                      chl::to_string(receiver_service_id));

            delete rdma_sender;
        }

        sender_tl_engine = other.get_sender_engine();
        receiver_service_id = other.get_receiver_service_id();

        try
        {
            rdma_sender = RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, receiver_service_id);
            LOG_TRACE("[ChunkExtractorRDMA] assingment: created rdma_sender for receiver_service {} ",
                      chl::to_string(receiver_service_id));
        }
        catch(...)
        {
            LOG_ERROR("[ChunkExtractorRDMA] assignment: failed to create rdma_sender for receiver_service {} ",
                      chl::to_string(receiver_service_id));
            rdma_sender = nullptr;
        }
    }

    return *this;
}


chronolog::StoryChunkExtractorRDMA::~StoryChunkExtractorRDMA()
{
    if(rdma_sender != nullptr)
    {
        LOG_TRACE("[ChunkExtractorRDMA] Destructor: deleting receiver_service {} ",
                  chl::to_string(receiver_service_id));
        delete rdma_sender;
    }
}

////

int chronolog::StoryChunkExtractorRDMA::complete_story_drain(chronolog::StoryId const& story_id)
{
    if(rdma_sender == nullptr)
    {
        LOG_ERROR("[ChunkExtractorRDMA] Failed to send story drain complete StoryID={}: null RDMA sender", story_id);
        return chl::CL_ERR_UNKNOWN;
    }
    return rdma_sender->notify_story_drain_complete(story_id);
}

////

int chronolog::StoryChunkExtractorRDMA::process_chunk(chronolog::StoryChunk* story_chunk)
{
    CL_PROFILE_REGION("keeper_flush");

    try
    {
        auto const total_start = std::chrono::high_resolution_clock::now();
        LOG_DEBUG("[ExtractorRDMA] tl::thread_id={} processing chunk StoryId={} {}-{} {}-{} eventCount {}",
                  thallium::thread::self_id(),
                  story_chunk->getStoryId(),
                  story_chunk->getChronicleName(),
                  story_chunk->getStoryName(),
                  story_chunk->getStartTime(),
                  story_chunk->getEndTime(),
                  story_chunk->getEventCount());

        if(rdma_sender == nullptr)
        {
            LOG_ERROR("[ChunkExtractorRDMA] Failed to transfer StoryChunk StoryId={} StartTime={}",
                      story_chunk->getStoryId(),
                      story_chunk->getStartTime());
            return chl::CL_ERR_UNKNOWN;
        }

        std::string serialized_story_chunk;
        auto const serialization_start = std::chrono::high_resolution_clock::now();
        {
            CL_PROFILE_REGION("serialization");
            if(envEnabled("CHRONOLOG_KEEPER_FAST_WIRE"))
            {
                chronolog::StoryChunkWireFormat::serialize(*story_chunk, serialized_story_chunk);
            }
            else if(envEnabled("CHRONOLOG_KEEPER_DIRECT_SERIALIZE"))
            {
                serialized_story_chunk.reserve(estimateSerializedStoryChunkBytes(*story_chunk));
                StringOutputBuffer output_buffer(serialized_story_chunk);
                std::ostream output_stream(&output_buffer);
                cereal::BinaryOutputArchive oarchive(output_stream);
                oarchive(*story_chunk);
            }
            else
            {
                std::ostringstream oss(std::ios::binary);
                cereal::BinaryOutputArchive oarchive(oss);
                oarchive(*story_chunk);
                serialized_story_chunk = oss.str();
            }
        }
        auto const serialization_end = std::chrono::high_resolution_clock::now();
        CL_PROFILE_COUNTER("append_bytes", serialized_story_chunk.size());

        int transfer_return;
        auto const transfer_start = std::chrono::high_resolution_clock::now();
        {
            CL_PROFILE_REGION("rpc_send");
            transfer_return = rdma_sender->transfer_serialized_story_chunk(serialized_story_chunk);
        }
        auto const transfer_end = std::chrono::high_resolution_clock::now();

        auto const serialization_us =
                std::chrono::duration_cast<std::chrono::nanoseconds>(serialization_end - serialization_start).count() /
                1000.0;
        auto const transfer_us =
                std::chrono::duration_cast<std::chrono::nanoseconds>(transfer_end - transfer_start).count() / 1000.0;
        auto const total_us =
                std::chrono::duration_cast<std::chrono::nanoseconds>(transfer_end - total_start).count() / 1000.0;
        LOG_INFO("[ChunkExtractorRDMA] Drain profile StoryID={} StartTime={} EndTime={} receiver={} eventCount={} "
                 "serializedBytes={} serialization_us={} drain_rpc_us={} total_us={}",
                 story_chunk->getStoryId(),
                 story_chunk->getStartTime(),
                 story_chunk->getEndTime(),
                 chl::to_string(receiver_service_id),
                 story_chunk->getEventCount(),
                 serialized_story_chunk.size(),
                 serialization_us,
                 transfer_us,
                 total_us);

        if(transfer_return == chl::CL_SUCCESS)
        {
            LOG_INFO("[ChunkExtractorRDMA] Transfered StoryChunk StoryId={} StartTime={}",
                     story_chunk->getStoryId(),
                     story_chunk->getStartTime());
        }
        else
        {
            LOG_ERROR("[ChunkExtractorRDMA] Failed to transfer StoryChunk StoryId={} StartTime={}",
                      story_chunk->getStoryId(),
                      story_chunk->getStartTime());
        }

        return transfer_return;
    }
    catch(cereal::Exception const& ex)
    {
        LOG_ERROR("[RDMATransferAgent] Cereal exception while serializing StoryChunk StoryId={} StartTime={} ex {}",
                  story_chunk->getStoryId(),
                  story_chunk->getStartTime(),
                  ex.what());
    }
    catch(std::exception const& ex)
    {
        LOG_ERROR("[RDMATransferAgent] Standard exception while serializing StoryChunk StoryId={} StartTime={} ex {}",
                  story_chunk->getStoryId(),
                  story_chunk->getStartTime(),
                  ex.what());
    }
    catch(...)
    {
        LOG_ERROR("[ChunkExtractorRDMA] Exception while  transferring StoryChunkiStoryId={} StartTime={}",
                  story_chunk->getStoryId(),
                  story_chunk->getStartTime());
    }

    return chl::CL_ERR_STORY_CHUNK_EXTRACTION;
}
