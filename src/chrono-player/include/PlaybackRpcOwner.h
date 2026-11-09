#ifndef CHRONOLOG_PLAYBACK_RPC_OWNER_H
#define CHRONOLOG_PLAYBACK_RPC_OWNER_H
#include <RpcRuntime.h>
#include <NativeRuntime.h>
#include <thread>
#include <private/RpcProviderRegistration.h>
#include <private/RpcBulkLifetime.h>
#include <private/ServiceLifecycle.h>
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
        , registrations_(std::make_unique<rpc::RpcProviderRegistration>(*engine))
    {}
    static std::shared_ptr<PlaybackRpcOwner> Prepare(std::shared_ptr<rpc::RpcRuntime> const& runtime,
                                                     RpcTimeoutPolicy const& policy,
                                                     std::unique_ptr<rpc::NativeMidRuntime> native)
    {
        auto owner = std::shared_ptr<PlaybackRpcOwner>(new PlaybackRpcOwner(policy));
        owner->native_ = std::move(native);
        owner->runtime = runtime;
        runtime->retainOwner(owner,
                             [owner]
                             {
                                 while(!owner->retirementReady.load(std::memory_order_acquire) ||
                                       !owner->entriesIdle() || owner->hotPermits.used() ||
                                       owner->queryPermits.used() || owner->responseSources.used())
                                     std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                 auto mid = owner->engine ? owner->engine->get_margo_instance() : MARGO_INSTANCE_NULL;
                                 owner->native_->cleanup();
                                 {
                                     std::lock_guard<std::mutex> lock(registryMutex());
                                     auto found = registry().find(mid);
                                     if(found != registry().end() && found->second.lock().get() == owner.get())
                                         registry().erase(found);
                                 }
                                 owner->provider.reset();
                                 owner->hotFetch = {};
                                 owner->receiveResponse = {};
                                 owner->responseAvailable = {};
                                 owner->registrations_.reset();
                                 owner->engine.reset();
                                 owner->dependencies.reset();
                                 owner->cleanupComplete.store(true, std::memory_order_release);
                                 if(owner->afterCleanup)
                                     owner->afterCleanup();
                             });
        return owner;
    }
    int initialize(rpc::NativeMidSpec spec)
    {
        if(bindingAttempted_)
            return bindingStatus_;
        bindingAttempted_ = true;
        bindingStatus_ = native_->initialize(std::move(spec));
        if(bindingStatus_ != CL_SUCCESS)
            return bindingStatus_;
        bindingStatus_ = CL_ERR_UNKNOWN;
        engine = native_->engine();
        registrations_ = std::make_unique<rpc::RpcProviderRegistration>(*engine);
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            auto& old = registry()[engine->get_margo_instance()];
            if(!old.expired())
                throw std::logic_error("Playback MID already has an owner");
            old = shared_from_this();
        }
        initializeProcedure();
        bindingStatus_ = CL_SUCCESS;
        return bindingStatus_;
    }
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
        return registrations_ ? registrations_->provision("receive_query_response", provider, deadline)
                              : CL_ERR_SHUTDOWN_INCOMPLETE;
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
        if(!lock.try_lock_until(deadline.expiresAt()) || deadline.expired() || closing())
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
    // Close new entries, join all older reserve/copy/submit paths, then prove
    // original C6 work and native member cleanup settled. The fact stays true.
    bool quiesceQueries(rpc::RpcDeadline const& deadline, std::function<void()> const& afterIdle = {})
    {
        quiesce();
        if(queriesSettled.load(std::memory_order_acquire))
            return true;
        auto actual = runtime.lock();
        if(!actual)
            return false;
        while(!entriesIdle())
        {
            if(deadline.expired())
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::unique_lock<std::timed_mutex> lock(admissionMutex, std::defer_lock);
            if(!lock.try_lock_until(deadline.expiresAt()) || deadline.expired())
                return false;
        }
        if(!rpc::allControlIdle(*actual, 6, deadline))
            return false;
        while(hotPermits.used() || queryPermits.used() || responseSources.used())
        {
            if(deadline.expired())
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(afterIdle)
            afterIdle();
        queriesSettled.store(true, std::memory_order_release);
        return true;
    }
    std::atomic<bool> queriesSettled{false};
    int provisionHotFetch(uint16_t provider, rpc::RpcDeadline const& deadline)
    {
        return registrations_ ? registrations_->provision("story_range_fetch", provider, deadline)
                              : CL_ERR_SHUTDOWN_INCOMPLETE;
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
    bool entriesIdle() const noexcept { return entries_.load(std::memory_order_acquire) == Closed; }
    std::atomic<bool> retirementReady{false}, cleanupComplete{false};
    std::shared_ptr<thallium::engine> engine;
    thallium::remote_procedure hotFetch, receiveResponse, responseAvailable;
    rpc::BulkPermits hotPermits, queryPermits, responseSources;
    std::timed_mutex admissionMutex;
    std::shared_ptr<void> dependencies;
    std::shared_ptr<void> provider;
    std::weak_ptr<rpc::RpcRuntime> runtime;
    RpcTimeoutPolicy const policy;
    // Set before this owner's runtime is closed. Runs std-only on the cleanup
    // worker after the physical native cleanup; the Player writes its Stopped
    // record here. The runtime's drain stays incomplete until it returns.
    std::function<void()> afterCleanup;

private:
    explicit PlaybackRpcOwner(RpcTimeoutPolicy policy)
        : policy(policy)
    {}
    std::unique_ptr<rpc::NativeMidRuntime> native_;
    bool bindingAttempted_ = false;
    int bindingStatus_ = CL_ERR_UNKNOWN;
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
    std::unique_ptr<rpc::RpcProviderRegistration> registrations_;
};
} // namespace chronolog
#endif
