#ifndef RDMA_TRANSFER_AGENT_H
#define RDMA_TRANSFER_AGENT_H

#include <string>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <ServiceId.h>

namespace tl = thallium;

namespace chronolog
{

//class StoryChunk;

class RDMATransferAgent
{
public:
    static RDMATransferAgent* CreateRDMATransferAgent(tl::engine& tl_engine, ServiceId const& receiver_service_id)
    {
        RDMATransferAgent* rdma_transfer_agent = nullptr;
        try
        {
            rdma_transfer_agent = new RDMATransferAgent(tl_engine, receiver_service_id);
        }
        catch(tl::exception const& ex)
        {
            LOG_ERROR("[RDMATransferAgent] Failed to create RDMATransferAgent for receiver {}",
                      chronolog::to_string(receiver_service_id));
            rdma_transfer_agent = nullptr;
        }
        return rdma_transfer_agent;
    }


    ~RDMATransferAgent();

    int transfer_serialized_story_chunk(std::string const& chunk);
    bool is_receiver_available() const;

private:
    tl::engine& service_engine;                  // local tl::engine
    ServiceId receiver_service_id;               // remote receiver service ServiceId
    tl::provider_handle receiver_service_handle; // tl::provider_handle for remote receiver service
    tl::remote_procedure receiver_available;
    tl::remote_procedure receive_story_chunk;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    RDMATransferAgent(tl::engine& tl_engine, ServiceId const& receiver_service_id);
};


} // namespace chronolog


#endif
