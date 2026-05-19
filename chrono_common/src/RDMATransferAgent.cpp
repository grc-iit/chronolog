#include <thallium/serialization/stl/vector.hpp>
#include <cereal/archives/binary.hpp>

#include <atomic>
#include <chrono>

#include <chrono_monitor.h>
#include <chronolog_profile.h>
#include <chronolog_errcode.h>
#include <RDMATransferAgent.h>

namespace tl = thallium;
namespace chl = chronolog;

namespace
{
std::atomic<uint64_t> transfer_sequence{0};
}

chronolog::RDMATransferAgent::RDMATransferAgent(tl::engine& tl_engine, chronolog::ServiceId const& service_id)
    : service_engine(tl_engine)
    , receiver_service_id(service_id)
{
    std::string service_addr_string;
    receiver_service_id.get_service_as_string(service_addr_string);

    receiver_service_handle =
            tl::provider_handle(service_engine.lookup(service_addr_string), receiver_service_id.getProviderId());

    receiver_is_available = service_engine.define("receiver_is_available");
    receive_story_chunk = service_engine.define("receive_story_chunk");
    story_drain_complete = service_engine.define("story_drain_complete");

    LOG_DEBUG("[RDMATransferAgent] created agent for receiver service {}", chl::to_string(receiver_service_id));
}

chronolog::RDMATransferAgent::~RDMATransferAgent()
{
    LOG_DEBUG("[RDMATransferAgent] Destroying agent for receiver service {}", chl::to_string(receiver_service_id));
    receiver_is_available.deregister();
    receive_story_chunk.deregister();
    story_drain_complete.deregister();
}

bool chronolog::RDMATransferAgent::is_receiver_available() const
{
    bool ret_value = false;
    try
    {
        ret_value = receiver_is_available.on(receiver_service_handle)();
    }
    catch(...)
    {
        LOG_ERROR("[RDMATransferAgent] Unknown exception in rpc with receiver_service {}.",
                  chl::to_string(receiver_service_id));
    }

    LOG_DEBUG("[RDMATransferAgent] receiver_service {} is available {}",
              chl::to_string(receiver_service_id),
              ret_value);
    return ret_value;
}

///////////////////////////////////
int chronolog::RDMATransferAgent::notify_story_drain_complete(StoryId const& story_id)
{
    try
    {
        auto const call_start = std::chrono::high_resolution_clock::now();
        int status = story_drain_complete.on(receiver_service_handle)(story_id);
        auto const call_end = std::chrono::high_resolution_clock::now();
        auto const call_us =
                std::chrono::duration_cast<std::chrono::nanoseconds>(call_end - call_start).count() / 1000.0;
        LOG_INFO("[RDMATransferAgent] Story drain complete profile receiver={} StoryID={} status={} rpc_us={}",
                 chl::to_string(receiver_service_id),
                 story_id,
                 status,
                 call_us);
        return status;
    }
    catch(tl::exception const& ex)
    {
        LOG_ERROR("[RDMATransferAgent] Thallium exception while sending story drain complete StoryID={} ex={}",
                  story_id,
                  ex.what());
    }
    catch(...)
    {
        LOG_ERROR("[RDMATransferAgent] Unknown exception while sending story drain complete StoryID={}", story_id);
    }

    return chronolog::CL_ERR_UNKNOWN;
}

///////////////////////////////////
int chronolog::RDMATransferAgent::transfer_serialized_story_chunk(std::string const& serialized_story_chunk)
{
    CL_PROFILE_REGION("rpc_send");
    CL_PROFILE_COUNTER("append_bytes", serialized_story_chunk.size());

    try
    {
        auto const total_start = std::chrono::high_resolution_clock::now();
        uint64_t const transfer_id = transfer_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        std::vector<std::pair<void*, std::size_t>> segments(1);
        segments[0].first = (void*)(serialized_story_chunk.data());
        segments[0].second = serialized_story_chunk.size();
        auto const expose_start = std::chrono::high_resolution_clock::now();
        tl::bulk tl_bulk = service_engine.expose(segments, tl::bulk_mode::read_only);
        auto const expose_end = std::chrono::high_resolution_clock::now();
        LOG_TRACE("[RDMATransferAgent] about to transfer Chunk size: {}  tl_bulk size {}",
                  serialized_story_chunk.size(),
                  tl_bulk.size());

        size_t bytes_transfered;
        auto const call_start = std::chrono::high_resolution_clock::now();
        {
            CL_PROFILE_REGION("rpc_send");
            bytes_transfered = receive_story_chunk.on(receiver_service_handle)(tl_bulk);
        }
        auto const call_end = std::chrono::high_resolution_clock::now();

        auto const expose_us =
                std::chrono::duration_cast<std::chrono::nanoseconds>(expose_end - expose_start).count() / 1000.0;
        auto const call_us =
                std::chrono::duration_cast<std::chrono::nanoseconds>(call_end - call_start).count() / 1000.0;
        auto const total_us =
                std::chrono::duration_cast<std::chrono::nanoseconds>(call_end - total_start).count() / 1000.0;
        LOG_INFO("[RDMATransferAgent] Transfer profile transferId={} receiver={} bytes={} tlBulkBytes={} "
                 "transferredBytes={} expose_us={} remote_call_us={} total_us={}",
                 transfer_id,
                 chl::to_string(receiver_service_id),
                 serialized_story_chunk.size(),
                 tl_bulk.size(),
                 bytes_transfered,
                 expose_us,
                 call_us,
                 total_us);

        LOG_DEBUG("[RDMATransferAgent] prepared tl_bulk size {} transfered {} bytes", tl_bulk.size(), bytes_transfered);

        if(bytes_transfered == tl_bulk.size())
        {
            LOG_TRACE("[RDMATransferAgent] Successfully transfered bulk");
            return chronolog::CL_SUCCESS;
        }
    }
    catch(tl::exception const& ex)
    {
        LOG_ERROR("[RDMATransferAgent] Thallium exception while transferring bulk: {}", ex.what());
    }
    catch(...)
    {
        LOG_ERROR("[RDMATransferAgent] Unknown exception while  transferring bulk.");
    }

    return chronolog::CL_ERR_STORY_CHUNK_EXTRACTION;
}
