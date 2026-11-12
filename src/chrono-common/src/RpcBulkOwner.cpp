#include <private/RpcBulkOwner.h>
#include <map>
#include <mutex>
#include <thread>
namespace chronolog::rpc
{
namespace
{
std::mutex ownersMutex;
std::map<margo_instance_id, std::weak_ptr<RpcBulkOwner>> owners;
} // namespace
std::shared_ptr<RpcBulkOwner> RpcBulkOwner::Find(margo_instance_id mid)
{
    std::lock_guard<std::mutex> lock(ownersMutex);
    auto found = owners.find(mid);
    return found == owners.end() ? nullptr : found->second.lock();
}
std::shared_ptr<RpcBulkOwner> RpcBulkOwner::Create(std::shared_ptr<thallium::engine> native,
                                                   std::shared_ptr<RpcRuntime> const& ownerRuntime,
                                                   RpcTimeoutPolicy const& policy,
                                                   bool outbound)
{
    auto owner = std::shared_ptr<RpcBulkOwner>(new RpcBulkOwner(std::move(native), policy));
    owner->registerActualOwner();
    owner->runtime = ownerRuntime;
    ownerRuntime->retainOwner(owner,
                              [owner]
                              {
                                  // B12 must prove incoming/native settlement before explicit native
                                  // destruction. The main error/stop gate keeps referenced stores alive.
                                  throw std::runtime_error("Bulk native retirement is not proven");
                              });
    if(outbound)
    {
        owner->receiveChunk = owner->engine->define("receive_story_chunk");
        owner->available = owner->engine->define("receiver_available");
    }
    return owner;
}
void RpcBulkOwner::registerActualOwner()
{
    std::lock_guard<std::mutex> lock(ownersMutex);
    auto& previous = owners[engine->get_margo_instance()];
    if(!previous.expired())
        throw std::logic_error("Bulk owner already exists for this MID");
    previous = shared_from_this();
}
std::shared_ptr<RpcBulkOwner> RpcBulkOwner::Prepare(std::shared_ptr<RpcRuntime> const& runtime,
                                                    RpcTimeoutPolicy const& policy,
                                                    bool outbound,
                                                    std::unique_ptr<NativeMidRuntime> native)
{
    if(!runtime || !native)
        throw std::invalid_argument("An actual bulk MID needs native/runtime ownership");
    auto owner = std::shared_ptr<RpcBulkOwner>(new RpcBulkOwner({}, policy));
    owner->runtime = runtime;
    owner->native_ = std::move(native);
    owner->outbound_ = outbound;
    runtime->retainOwner(
            owner,
            [owner]
            {
                // Transient preparation is retained, never thrown into permanent Core
                // quarantine. Public drain has its own independent finite deadline.
                while(!owner->retirementReady.load(std::memory_order_acquire) || owner->sources.used() != 0 ||
                      owner->incoming.used() != 0 || owner->pendingOutcomes.used() != 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                auto retiredMid = owner->engine ? owner->engine->get_margo_instance() : MARGO_INSTANCE_NULL;
                owner->native_->finalize();
                {
                    std::lock_guard<std::mutex> lock(ownersMutex);
                    auto found = owners.find(retiredMid);
                    if(found != owners.end() && found->second.lock().get() == owner.get())
                        owners.erase(found);
                }
                // The providers outlive Margo's incoming-handler fence. Each
                // thallium object holds a reference on the MID and Margo cleans
                // up only when the last one goes, so they are released now,
                // while Mercury is still there, and cleanup releases the last.
                owner->provider.reset();
                owner->watermarkReports.reset();
                owner->registrySession.reset();
                owner->registrations_.reset();
                owner->receiveChunk = {};
                owner->available = {};
                owner->engine.reset();
                owner->dependencies.reset();
                owner->native_->awaitApplicationReferences();
                owner->native_->cleanup();
                owner->cleanupComplete.store(true, std::memory_order_release);
                if(owner->afterCleanup)
                    owner->afterCleanup();
            });
    return owner;
}
int RpcBulkOwner::initialize(NativeMidSpec spec)
{
    if(!native_)
        return CL_ERR_INVALID_ARG;
    if(bindingAttempted_)
        return bindingStatus_;
    bindingAttempted_ = true;
    bindingStatus_ = native_->initialize(std::move(spec));
    if(bindingStatus_ != CL_SUCCESS)
        return bindingStatus_;
    bindingStatus_ = CL_ERR_UNKNOWN;
    engine = native_->engine();
    registrations_ = std::make_unique<RpcProviderRegistration>(*engine);
    registerActualOwner();
    if(outbound_)
    {
        receiveChunk = engine->define("receive_story_chunk");
        available = engine->define("receiver_available");
    }
    bindingStatus_ = CL_SUCCESS;
    return bindingStatus_;
}
bool RpcBulkOwner::consumeKnownOutcomes(RpcDeadline const& deadline)
{
    if(!retirementReady.load(std::memory_order_acquire))
        return false;
    std::unique_lock<std::timed_mutex> lock(pendingMutex, std::defer_lock);
    if(!lock.try_lock_until(deadline.expiresAt()) || deadline.expired())
        return false;
    for(auto const& item: chunks)
        if(!item.second->state->known.load(std::memory_order_acquire))
            return false;
    // All data is already safe at retirementReady. Signal existing charged
    // workers to release known outcome snapshots; never destroy them here.
    for(auto const& item: chunks) item.second->consumed.store(true, std::memory_order_release);
    return true;
}
} // namespace chronolog::rpc
