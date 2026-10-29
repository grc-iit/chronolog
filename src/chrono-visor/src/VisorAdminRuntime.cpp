#include <VisorAdminRuntime.h>
#include <DataStoreAdminClient.h>
#include <KeeperRegistry.h>
#include <stdexcept>
namespace chronolog
{
namespace
{
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
                                     std::shared_ptr<AdminLookup> dependency)
    : policy(p)
    , runtime(capacity)
    , lookup(dependency ? std::move(dependency) : std::make_shared<NativeAdminLookup>())
{
    if(validateRpcTimeoutPolicy(p) != CL_SUCCESS)
        throw std::invalid_argument("invalid Visor RPC policy");
}
void VisorAdminRuntime::initializeRpcDefinitions()
{
    // Define once on the initialization thread. Concurrent duplicate definitions
    // can replace Margo registration data and leak it on the pinned stack.
    constexpr char const* names[] = {"start_story_recording",
                                     "start_story_recording_with_keepers",
                                     "stop_story_recording",
                                     "destroy_story",
                                     "destroy_chronicle",
                                     "shutdown_data_collection",
                                     "collection_service_available"};
    for(std::size_t i = 0; i < procedures.size(); ++i) procedures[i] = engine->define(names[i]);
}
int DataStoreAdminClient::shutdown_collection(std::shared_ptr<KeeperRegistry> registry,
                                              rpc::RpcDeadline const& deadline)
{
    auto token = owner_->runtime.reserve(deadline);
    if(!token)
        return token.status();
    auto result = owner_->runtime.invoke<int>(
            std::move(token),
            std::make_shared<AdminWork>(shared_from_this(),
                                        AdminArguments{AdminAction::Shutdown, "", "", 0, 0, {}},
                                        std::make_shared<AdminCompletion>(),
                                        std::move(registry)));
    // The shipped shutdown handlers acknowledge with wire value 1. Zero is a
    // false ACK here, even though local status APIs use CL_SUCCESS == 0.
    if(result.reply)
        return *result.reply == 1 ? CL_SUCCESS : CL_ERR_SHUTDOWN_INCOMPLETE;
    return result.status;
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
rpc::RpcResult<int> AdminWork::run(rpc::OperationContext& context)
{
    rpc::RpcResult<int> result{CL_ERR_OUTCOME_UNCONFIRMED, true, std::nullopt};
    int exceptionKind = 0;
    int nativeError = HG_SUCCESS;
    try
    {
        result = client_->execute(context, arguments_);
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
    completion_->done.store(true, std::memory_order_release);
    // Last operation at adapter exit: native invocation and local preparation
    // have returned; this is deliberately NOT a settlement assertion.
    if(arguments_.action == AdminAction::Shutdown && !result.reply)
        ++client_->owner_->observation->shutdownNoReplyRunReturns;
    return result;
}
} // namespace chronolog
