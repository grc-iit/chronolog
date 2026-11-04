#ifndef RDMA_TRANSFER_AGENT_H
#define RDMA_TRANSFER_AGENT_H
#include <string>
#include <map>
#include <mutex>
#include <tuple>
#include <memory>
#include <thallium.hpp>
#include <ChunkReceipt.h>
#include <chronolog_types.h>
#include <ServiceId.h>
#include <private/RpcBulkOwner.h>
#include <private/RpcBulkSend.h>
namespace tl = thallium;
namespace chronolog
{
class StoryChunk;
class RDMATransferAgent
{
public:
    static RDMATransferAgent* CreateRDMATransferAgent(tl::engine& engine, ServiceId const& route)
    {
        return CreateRDMATransferAgent(rpc::RpcBulkOwner::Find(engine.get_margo_instance()), route);
    }
    static RDMATransferAgent* CreateRDMATransferAgent(std::shared_ptr<rpc::RpcBulkOwner> owner, ServiceId const& route)
    {
        if(!owner || !route.is_valid() || owner->closing || owner->runtime.expired())
            return nullptr;
        return new RDMATransferAgent(std::move(owner), route);
    }
    ~RDMATransferAgent() = default;
    int transfer_serialized_story_chunk(std::string const&,
                                        ServiceId const& reporter = {},
                                        ChunkReceipt* receipt = nullptr);
    int transfer_story_chunk(StoryChunk const&, ServiceId const& reporter = {}, ChunkReceipt* receipt = nullptr);
    bool is_receiver_available() const;

private:
    RDMATransferAgent(std::shared_ptr<rpc::RpcBulkOwner> owner, ServiceId route)
        : owner_(std::move(owner))
        , route_(std::move(route))
    {}
    // Borrowed input is copied once after admission. That O(input-size)
    // CPU/allocation boundary is not independently preemptible; elapsed time
    // consumes the original deadline. No worker retains a raw caller input.
    std::shared_ptr<rpc::RpcBulkOwner> owner_;
    ServiceId route_;
};
} // namespace chronolog
#endif
