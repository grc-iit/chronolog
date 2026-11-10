#ifndef CHRONOLOG_RPC_BULK_OWNER_H
#define CHRONOLOG_RPC_BULK_OWNER_H
#include <RpcRuntime.h>
#include <NativeRuntime.h>
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
    // Prepare the actual owner before admitting construction or exposing a provider.
    static std::shared_ptr<RpcBulkOwner> Prepare(std::shared_ptr<RpcRuntime> const& runtime,
                                                 RpcTimeoutPolicy const& policy,
                                                 bool outbound,
                                                 std::unique_ptr<NativeMidRuntime> native);
    int initialize(NativeMidSpec spec);
    bool consumeKnownOutcomes(RpcDeadline const& deadline);
    static std::shared_ptr<RpcBulkOwner> Find(margo_instance_id mid);
    int provision(char const* name, uint16_t provider, RpcDeadline const& deadline)
    {
        return registrations_ ? registrations_->provision(name, provider, deadline) : CL_ERR_SHUTDOWN_INCOMPLETE;
    }
    class Entry
    {
        std::shared_ptr<RpcBulkOwner> owner_;

    public:
        explicit Entry(std::shared_ptr<RpcBulkOwner> owner)
            : owner_(std::move(owner))
        {}
        Entry(Entry const&) = delete;
        Entry(Entry&&) = default;
        ~Entry()
        {
            if(owner_)
                owner_->publicEntries_.fetch_sub(1, std::memory_order_acq_rel);
        }
    };
    std::optional<Entry> enterPublic()
    {
        auto observed = publicEntries_.load(std::memory_order_acquire);
        while(!(observed & Closed) && observed != Closed - 1)
            if(publicEntries_.compare_exchange_weak(observed, observed + 1, std::memory_order_acq_rel))
                return Entry(shared_from_this());
        return std::nullopt;
    }
    // This fences public data access only; Margo's final native fence is still
    // required for queued/rejected handlers and post-wrapper request cleanup.
    void quiescePublic() noexcept { publicEntries_.fetch_or(Closed, std::memory_order_acq_rel); }
    void quiesce() noexcept
    {
        quiescePublic();
        closing.store(true, std::memory_order_release);
    }
    bool publicIdle() const noexcept { return publicEntries_.load(std::memory_order_acquire) == Closed; }
    std::shared_ptr<thallium::engine> engine;
    std::weak_ptr<RpcRuntime> runtime;
    RpcTimeoutPolicy const policy;
    std::shared_ptr<void> dependencies;
    std::shared_ptr<void> provider;
    std::shared_ptr<void> registrySession;
    std::shared_ptr<void> watermarkReports; // One publisher Core per actual data-admin MID.
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
        // The serialized chunk this transfer sent; a later transfer of the
        // same bytes collects its receipt.
        std::shared_ptr<SerializedStoryChunk const> payload;
        explicit ChunkOutcome(BulkPermits::Permit metadata)
            : permit(std::move(metadata))
        {}
    };
    std::timed_mutex pendingMutex;
    std::multimap<ChunkIdentity, std::shared_ptr<ChunkOutcome>> chunks;
    // Private test-only preparation barrier, installed before any work.
    std::function<void()> beforeSerialize;
    std::function<void()> beforeDispatch;
    BulkReceives incoming;
    std::atomic<bool> closing{false};
    std::atomic<bool> writeAdmitted{false}; // Monotonic; direct dataful stop needs a safe publisher barrier.
    // Set only after component data/dependency progress no longer needs this MID.
    std::atomic<bool> retirementReady{false};
    std::atomic<bool> cleanupComplete{false};
    // Set before this owner's runtime is closed. Runs std-only on its cleanup
    // worker after the physical native cleanup; the daemon's last original
    // owner writes its Stopped record here. It may block that worker, and the
    // runtime's drain stays incomplete until it returns.
    std::function<void()> afterCleanup;

private:
    RpcBulkOwner(std::shared_ptr<thallium::engine> native, RpcTimeoutPolicy const& policy)
        : engine(std::move(native))
        , policy(policy)
    {
        if(engine)
            registrations_ = std::make_unique<RpcProviderRegistration>(*engine);
    }
    void registerActualOwner();
    static constexpr unsigned Closed = 1u << 31;
    std::atomic<unsigned> publicEntries_{0};
    bool outbound_ = false;
    std::unique_ptr<NativeMidRuntime> native_;
    bool bindingAttempted_ = false;
    int bindingStatus_ = CL_ERR_UNKNOWN;
    std::unique_ptr<RpcProviderRegistration> registrations_;
};
} // namespace chronolog::rpc
#endif
