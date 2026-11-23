#include <VisorAdminRuntime.h>
#include <DataStoreAdminClient.h>
#include <KeeperRegistry.h>
#include <stdexcept>
#include <thread>
#include <algorithm>
namespace chronolog
{
namespace
{
constexpr char const* names[] = {"start_story_recording",
                                 "start_story_recording_with_keepers",
                                 "stop_story_recording",
                                 "destroy_story",
                                 "destroy_chronicle",
                                 "shutdown_data_collection",
                                 "collection_service_available"};
class NativeAdminLookup final: public AdminLookup
{
public:
    thallium::endpoint resolve(thallium::engine& engine, std::string const& address) override
    {
        return engine.lookup(address);
    }
};
} // namespace
VisorAdminRuntime::VisorAdminRuntime(RpcTimeoutPolicy const& p,
                                     std::size_t capacity,
                                     std::shared_ptr<AdminLookup> dependency,
                                     std::shared_ptr<rpc::RpcRuntime> actual)
    : policy(p)
    , runtime(capacity, actual)
    , lookup(dependency ? std::move(dependency) : std::make_shared<NativeAdminLookup>())
{
    if(validateRpcTimeoutPolicy(p) != CL_SUCCESS)
        throw std::invalid_argument("invalid Visor RPC policy");
}
void VisorAdminRuntime::initializeRpcDefinitions()
{
    // Define once on the initialization thread. Concurrent duplicate definitions
    // can replace Margo registration data and leak it on the pinned stack.
    for(std::size_t i = 0; i < procedures.size(); ++i) procedures[i] = engine->define(names[i]);
    registrations_ = std::make_unique<rpc::RpcProviderRegistration>(*engine);
}
void VisorAdminRuntime::releaseNativeDescriptors()
{
    registrations_.reset();
    procedures = {};
    engine.reset();
}
int VisorAdminRuntime::provision(AdminAction action, uint16_t provider, rpc::RpcDeadline const& deadline)
{
    auto index = static_cast<std::size_t>(action);
    if(!registrations_ || index >= procedures.size())
        return CL_ERR_UNKNOWN;
    return registrations_->provision(names[index], provider, deadline);
}
int DataStoreAdminClient::shutdown_collection(std::shared_ptr<KeeperRegistry> registry,
                                              rpc::RpcDeadline const& deadline)
{
    auto entry = rpc::RpcDeadline::Clock::now();
    auto attemptDeadline = rpc::RpcDeadline::fromStart(entry, std::chrono::milliseconds(owner_->policy.attempt_ms));
    auto waitDeadline = deadline.expiresAt() < attemptDeadline.expiresAt() ? deadline : attemptDeadline;
    std::shared_ptr<AdminCompletion> completion;
    {
        std::unique_lock<std::timed_mutex> lock(shutdownMutex_, std::defer_lock);
        if(!lock.try_lock_until(waitDeadline.expiresAt()) || waitDeadline.expired())
            return CL_ERR_SHUTDOWN_INCOMPLETE;
        // Only a proved pre-dispatch expiry permits another preparation.
        if(shutdown_ && shutdown_->done.load(std::memory_order_acquire) && !shutdown_->confirmed &&
           !shutdown_->dispatched && shutdown_->status == CL_ERR_DEADLINE_EXCEEDED)
            shutdown_.reset();
        if(!shutdown_)
        {
            auto token = owner_->runtime.reserve(waitDeadline);
            if(!token)
                return CL_ERR_SHUTDOWN_INCOMPLETE;
            auto prepared = std::make_shared<AdminCompletion>();
            auto work = std::make_shared<AdminWork>(shared_from_this(),
                                                    AdminArguments{AdminAction::Shutdown, "", "", 0, 0, {}},
                                                    prepared,
                                                    std::move(registry));
            owner_->runtime.submit<int>(std::move(token), std::move(work)).releaseToRuntime();
            shutdown_ = std::move(prepared);
        }
        completion = shutdown_;
    }
    while(!completion->done.load(std::memory_order_acquire) && !waitDeadline.expired())
        std::this_thread::sleep_for(
                std::min(waitDeadline.remaining(),
                         std::chrono::duration_cast<rpc::RpcDeadline::Clock::duration>(std::chrono::milliseconds(1))));
    // Raw ACK1 follows the actual remote join. Unknown/missing/false ACKs never
    // permit replacement work, data release or dependency shutdown.
    return completion->done.load(std::memory_order_acquire) && completion->confirmed && completion->status == 1
                   ? CL_SUCCESS
                   : CL_ERR_SHUTDOWN_INCOMPLETE;
}
AdminWork::AdminWork(std::shared_ptr<DataStoreAdminClient> client,
                     AdminArguments args,
                     std::shared_ptr<AdminCompletion> completion,
                     std::shared_ptr<KeeperRegistry> registry)
    : client_(std::move(client))
    , arguments_(std::move(args))
    , completion_(std::move(completion))
    , registry_(std::move(registry))
{
    if(!client_ || !registry_ || registry_->adminOwner().get() != client_->owner_.get())
        throw std::invalid_argument("AdminWork requires its actual shared registry owner");
}
AdminWork::~AdminWork()
{
    if(!entered_)
    {
        completion_->status = CL_ERR_DEADLINE_EXCEEDED;
        completion_->done.store(true, std::memory_order_release);
        if(completion_->finished)
            completion_->finished(*completion_);
    }
}
rpc::RpcResult<int> AdminWork::run(rpc::OperationContext& context)
{
    entered_ = true;
    rpc::RpcResult<int> result{CL_ERR_OUTCOME_UNCONFIRMED, true, std::nullopt};
    int exceptionKind = 0;
    int nativeError = HG_SUCCESS;
    try
    {
        result = client_->execute(context, arguments_, native_);
    }
    catch(thallium::timeout const&)
    {
        exceptionKind = 1;
        nativeError = HG_TIMEOUT;
    }
    catch(thallium::margo_exception const& error)
    {
        exceptionKind = 2;
        nativeError = error.error();
    }
    catch(thallium::exception const&)
    {
        exceptionKind = 3;
    }
    catch(std::exception const&)
    {
        exceptionKind = 4;
    }
    catch(...)
    {
        exceptionKind = 5;
    }
    if(arguments_.action == AdminAction::Shutdown && !result.reply)
    {
        client_->owner_->observation->shutdownExceptionKind.store(exceptionKind, std::memory_order_relaxed);
        client_->owner_->observation->nativeShutdownError.store(nativeError, std::memory_order_relaxed);
    }
    completion_->status = result.reply ? *result.reply : result.status;
    completion_->confirmed = result.reply.has_value();
    completion_->dispatched = result.may_have_dispatched;
    completion_->done.store(true, std::memory_order_release);
    if(completion_->finished)
        completion_->finished(*completion_);
    // Last operation at adapter exit: native invocation and local preparation
    // have returned; this is deliberately NOT a settlement assertion.
    if(arguments_.action == AdminAction::Shutdown && !result.reply)
        ++client_->owner_->observation->shutdownNoReplyRunReturns;
    return result;
}
} // namespace chronolog
