#ifndef CHRONOLOG_RPC_BULK_OWNER_H
#define CHRONOLOG_RPC_BULK_OWNER_H
#include <RpcRuntime.h>
#include <RpcPolicy.h>
#include <private/RpcBulkLifetime.h>
#include <private/RpcBulkSend.h>
#include <chronolog_types.h>
#include <StoryChunk.h>
#include <map>
#include <tuple>
#include <private/RpcProviderRegistration.h>
#include <thallium.hpp>
#include <memory>
#include <stdexcept>
namespace chronolog::rpc
{
// Actual service engine/provider root. The keeper extraction MID has one of
// these; recording MIDs each have their own incoming owner. Never per peer.
class RpcBulkOwner: public std::enable_shared_from_this<RpcBulkOwner>
{
public:
    static std::shared_ptr<RpcBulkOwner> Create(std::shared_ptr<thallium::engine> engine,
                                                std::shared_ptr<RpcRuntime> const& runtime,
                                                RpcTimeoutPolicy const& policy,
                                                bool outbound);
    static std::shared_ptr<RpcBulkOwner> Find(margo_instance_id mid);
    int provision(char const* name, uint16_t provider, RpcDeadline const& deadline)
    {
        return registrations_.provision(name, provider, deadline);
    }
    void quiesce() noexcept { closing.store(true, std::memory_order_release); }
    std::shared_ptr<thallium::engine> engine;
    std::weak_ptr<RpcRuntime> runtime;
    RpcTimeoutPolicy const policy;
    std::shared_ptr<void> dependencies;
    std::shared_ptr<void> provider;
    thallium::remote_procedure receiveChunk, available;
    BulkPermits sources, pendingOutcomes;
    using ChunkIdentity = std::tuple<std::string,
                                     uint32_t,
                                     uint16_t,
                                     uint16_t,
                                     std::string,
                                     uint32_t,
                                     uint16_t,
                                     uint16_t,
                                     StoryId,
                                     uint64_t,
                                     uint64_t>;
    struct ChunkOutcome
    {
        BulkPermits::Permit permit;
        std::shared_ptr<BulkReplyState<ChunkReceipt>> state = std::make_shared<BulkReplyState<ChunkReceipt>>();
        std::atomic<bool> consumed{false};
        std::shared_ptr<StoryChunk const> snapshot;
        explicit ChunkOutcome(BulkPermits::Permit metadata)
            : permit(std::move(metadata))
        {}
    };
    std::timed_mutex pendingMutex;
    std::multimap<ChunkIdentity, std::shared_ptr<ChunkOutcome>> chunks;
    // Private test-only preparation barrier, installed before any work.
    std::function<void()> beforeSerialize;
    BulkReceives incoming;
    std::atomic<bool> closing{false};

private:
    RpcBulkOwner(std::shared_ptr<thallium::engine> native, RpcTimeoutPolicy const& policy)
        : engine(std::move(native))
        , policy(policy)
        , registrations_(*engine)
    {}
    RpcProviderRegistration registrations_;
};
} // namespace chronolog::rpc
#endif
