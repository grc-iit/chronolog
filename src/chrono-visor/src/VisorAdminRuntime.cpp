#include <VisorAdminRuntime.h>
#include <DataStoreAdminClient.h>
#include <KeeperRegistry.h>
#include <stdexcept>
namespace chronolog
{
VisorAdminRuntime::VisorAdminRuntime(RpcTimeoutPolicy const& p, std::size_t capacity)
    : policy(p)
    , runtime(capacity)
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
int DataStoreAdminClient::shutdown_collection()
{
    auto deadline = rpc::RpcDeadline::after(std::chrono::milliseconds(owner_->policy.shutdown_ms));
    auto token = owner_->runtime.reserve(deadline);
    if(!token)
        return token.status();
    auto result = owner_->runtime.invoke<int>(
            std::move(token),
            std::make_shared<AdminWork>(shared_from_this(),
                                        AdminArguments{AdminAction::Shutdown, "", "", 0, 0, {}},
                                        std::make_shared<AdminCompletion>(),
                                        nullptr));
    return result.reply ? *result.reply : result.status;
}
AdminWork::AdminWork(std::shared_ptr<DataStoreAdminClient> client,
                     AdminArguments args,
                     std::shared_ptr<AdminCompletion> completion,
                     std::shared_ptr<KeeperRegistry> registry)
    : client_(std::move(client))
    , arguments_(std::move(args))
    , completion_(std::move(completion))
    , registry_(std::move(registry))
{}
rpc::RpcResult<int> AdminWork::run(rpc::OperationContext& context)
{
    rpc::RpcResult<int> result{CL_ERR_OUTCOME_UNCONFIRMED, true, std::nullopt};
    try
    {
        result = client_->execute(context, arguments_);
    }
    catch(thallium::timeout const&)
    {}
    catch(...)
    {}
    completion_->status = result.reply ? *result.reply : result.status;
    completion_->confirmed = result.reply.has_value();
    completion_->done.store(true, std::memory_order_release);
    return result;
}
} // namespace chronolog
