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
    
    receiver_service_handle = tl::provider_handle(service_engine.lookup(service_addr_string), receiver_service_id.getProviderId());

    receiver_is_available = service_engine.define("receiver_is_available");
    receive_story_chunk = service_engine.define("receive_story_chunk");

    LOG_DEBUG("[RDMATransferAgent] created agent for receiver service {}", chl::to_string(receiver_service_id));
}

chronolog::RDMATransferAgent::~RDMATransferAgent()
{
    LOG_DEBUG("[RDMATransferAgent] Destroying agent for receiver service {}", chl::to_string(receiver_service_id));
    receiver_is_available.deregister();
    receive_story_chunk.deregister();
}

bool chronolog::RDMATransferAgent::is_receiver_available() const
{
    bool ret_value = false;
    try
    {
        ret_value = receiver_is_available.on(receiver_service_handle)( );
    }
    catch(...)
    {
        LOG_ERROR("[RDMATransferAgent] Unknown exception in rpc with receiver_service {}.", chl::to_string(receiver_service_id));
    }

    LOG_DEBUG("[RDMATransferAgent] receiver_service {} is available {}", chl::to_string(receiver_service_id), ret_value);
return ret_value;    
}

///////////////////////////////////
int chronolog::RDMATransferAgent::transfer_serialized_story_chunk( std::string const& serialized_story_chunk)
{
    try
    {
        std::vector <std::pair <void*, std::size_t>> segments(1);
        segments[0].first = (void*)(serialized_story_chunk.data());
        segments[0].second = serialized_story_chunk.size();
        tl::bulk tl_bulk = service_engine.expose(segments, tl::bulk_mode::read_only);
        LOG_TRACE("[RDMATransferAgent] about to transfer Chunk size: {}  tl_bulk size {}",  serialized_story_chunk.size(), tl_bulk.size());

        size_t bytes_transfered = receive_story_chunk.on(receiver_service_handle)(tl_bulk);

        LOG_DEBUG("[RDMATransferAgent] prepared tl_bulk size {} transfered {} bytes", tl_bulk.size(), bytes_transfered);

        if(bytes_transfered == tl_bulk.size())
        {
            LOG_TRACE("[RDMATransferAgent] Successfully transfered bulk");
            return chronolog::CL_SUCCESS;
        }
    }
    catch(tl::exception const &ex)
    {
        LOG_ERROR("[RDMATransferAgent] Thallium exception while transferring bulk: {}", ex.what());
    }
    catch(...)
    {
        LOG_ERROR("[RDMATransferAgent] Unknown exception while  transferring bulk.");
    }

    return chronolog::CL_ERR_STORY_CHUNK_EXTRACTION;
}
