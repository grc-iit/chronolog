#ifndef CHRONOLOG_PLAYBACK_RPC_OWNER_H
#define CHRONOLOG_PLAYBACK_RPC_OWNER_H
#include <RpcRuntime.h>
#include <private/RpcProviderRegistration.h>
#include <RpcPolicy.h>
#include <thallium.hpp>
#include <atomic>
#include <memory>
#include <stdexcept>
namespace chronolog
{
// The actual player engine/provider owner. Main retains all referenced queues
// and stores behind its teardown gate; children retain this owner independently.
// Native incoming/bulk settlement is intentionally not claimed in B08.
class PlaybackRpcOwner: public std::enable_shared_from_this<PlaybackRpcOwner>
{
public:
    PlaybackRpcOwner(std::unique_ptr<thallium::engine> native, RpcTimeoutPolicy policy)
        : engine(std::move(native))
        , policy(policy)
        , registrations_(*engine)
    {}
    void attach(std::shared_ptr<rpc::RpcRuntime> const& ownerRuntime)
    {
        runtime = ownerRuntime;
        auto retained = shared_from_this();
        ownerRuntime->retainOwner(retained,
                                  [retained]
                                  {
                                      // Outbound idle alone cannot authorize provider/Margo destruction.
                                      // B10/B12 must supply the real incoming and bulk settlement proof.
                                      throw std::runtime_error("Player incoming native settlement is unproven");
                                  });
    }
    void initializeProcedure() { hotFetch = engine->define("story_range_fetch"); }
    int provisionHotFetch(uint16_t provider, rpc::RpcDeadline const& deadline)
    {
        return registrations_.provision("story_range_fetch", provider, deadline);
    }
    class Entry
    {
    public:
        explicit Entry(std::shared_ptr<PlaybackRpcOwner> owner)
            : owner_(std::move(owner))
        {}
        Entry(Entry const&) = delete;
        Entry(Entry&& other) noexcept
            : owner_(std::move(other.owner_))
        {}
        ~Entry()
        {
            if(owner_)
                owner_->entries_.fetch_sub(1, std::memory_order_acq_rel);
        }

    private:
        std::shared_ptr<PlaybackRpcOwner> owner_;
    };
    std::optional<Entry> enter()
    {
        auto observed = entries_.load(std::memory_order_acquire);
        while(!(observed & Closed) && observed != Closed - 1)
            if(entries_.compare_exchange_weak(observed, observed + 1, std::memory_order_acq_rel))
                return Entry(shared_from_this());
        return std::nullopt;
    }
    void quiesce() noexcept { entries_.fetch_or(Closed, std::memory_order_acq_rel); }
    bool closing() const noexcept { return (entries_.load(std::memory_order_acquire) & Closed) != 0; }
    std::unique_ptr<thallium::engine> engine;
    thallium::remote_procedure hotFetch;
    std::shared_ptr<void> provider;
    std::weak_ptr<rpc::RpcRuntime> runtime;
    RpcTimeoutPolicy const policy;

private:
    static constexpr unsigned Closed = 1u << 31;
    std::atomic<unsigned> entries_{0};
    rpc::RpcProviderRegistration registrations_;
};
} // namespace chronolog
#endif
