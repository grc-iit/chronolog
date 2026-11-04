#ifndef CHRONOLOG_PLAYBACK_RPC_OWNER_H
#define CHRONOLOG_PLAYBACK_RPC_OWNER_H
#include <RpcRuntime.h>
#include <private/RpcProviderRegistration.h>
#include <private/RpcBulkLifetime.h>
#include <map>
#include <mutex>
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
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            auto& old = registry()[engine->get_margo_instance()];
            if(!old.expired())
                throw std::logic_error("Playback owner already exists for MID");
            old = shared_from_this();
        }
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
    static std::shared_ptr<PlaybackRpcOwner> Find(margo_instance_id mid)
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        auto found = registry().find(mid);
        return found == registry().end() ? nullptr : found->second.lock();
    }
    void initializeProcedure()
    {
        hotFetch = engine->define("story_range_fetch");
        receiveResponse = engine->define("receive_query_response");
        responseAvailable = engine->define("receiver_is_available");
    }
    int provisionResponse(uint16_t provider, rpc::RpcDeadline const& deadline)
    {
        return registrations_.provision("receive_query_response", provider, deadline);
    }
    struct QueryAdmission
    {
        rpc::RpcReservation coordinator, response;
        rpc::BulkPermits::Permit queryPermit, sourcePermit;
    };
    std::optional<QueryAdmission> reserveQuery(rpc::RpcDeadline const& deadline)
    {
        auto sharedRuntime = runtime.lock();
        if(!sharedRuntime || closing())
            return std::nullopt;
        std::unique_lock<std::timed_mutex> lock(admissionMutex, std::defer_lock);
        if(!lock.try_lock_until(deadline.expiresAt()) || deadline.expired())
            return std::nullopt;
        auto query = queryPermits.acquire();
        if(!query)
            return std::nullopt;
        auto source = responseSources.acquire();
        if(!source)
            return std::nullopt;
        auto coordinator = sharedRuntime->reserve(deadline);
        if(!coordinator)
            return std::nullopt;
        auto response = sharedRuntime->reserve(deadline);
        if(!response)
            return std::nullopt;
        return QueryAdmission{std::move(coordinator), std::move(response), std::move(*query), std::move(*source)};
    }
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
    thallium::remote_procedure hotFetch, receiveResponse, responseAvailable;
    rpc::BulkPermits hotPermits, queryPermits, responseSources;
    std::timed_mutex admissionMutex;
    std::shared_ptr<void> dependencies;
    std::shared_ptr<void> provider;
    std::weak_ptr<rpc::RpcRuntime> runtime;
    RpcTimeoutPolicy const policy;

private:
    static std::map<margo_instance_id, std::weak_ptr<PlaybackRpcOwner>>& registry()
    {
        static std::map<margo_instance_id, std::weak_ptr<PlaybackRpcOwner>> owners;
        return owners;
    }
    static std::mutex& registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }
    static constexpr unsigned Closed = 1u << 31;
    std::atomic<unsigned> entries_{0};
    rpc::RpcProviderRegistration registrations_;
};
} // namespace chronolog
#endif
