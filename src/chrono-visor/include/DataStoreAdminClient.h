#ifndef DataStoreAdmin_CLIENT_H
#define DataStoreAdmin_CLIENT_H
#include <VisorAdminRuntime.h>
#include <chronolog_types.h>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
namespace chronolog
{
// Immutable descriptor. Lookup and native callable handles are created only inside
// admitted owned work; registration does not perform network work under its lock.
class DataStoreAdminClient: public std::enable_shared_from_this<DataStoreAdminClient>
{
public:
    static std::shared_ptr<DataStoreAdminClient>
    CreateDataStoreAdminClient(std::shared_ptr<VisorAdminRuntime> owner, std::string address, uint16_t provider)
    {
        return std::shared_ptr<DataStoreAdminClient>(
                new DataStoreAdminClient(std::move(owner), std::move(address), provider));
    }
    rpc::RpcResult<int> execute(rpc::OperationContext& context, AdminArguments const& args)
    {
        auto endpoint = owner_->lookup->resolve(*owner_->engine, address_);
        auto remote = owner_->procedures[static_cast<std::size_t>(args.action)];
        thallium::provider_handle handle(endpoint, provider_);
        auto call = remote.on(handle);
        auto attempt = context.deadline().cappedAttempt(std::chrono::milliseconds(owner_->policy.attempt_ms));
        if(!attempt || !context.tryBeginDispatch())
            return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
        int reply;
        switch(args.action)
        {
            case AdminAction::Start:
                reply = call.timed(*attempt, args.chronicle, args.story, args.id, args.start);
                break;
            case AdminAction::StartPlayer:
                reply = call.timed(*attempt, args.chronicle, args.story, args.id, args.start, args.keepers);
                break;
            case AdminAction::Stop:
                reply = call.timed(*attempt, args.id);
                break;
            case AdminAction::DestroyStory:
                reply = call.timed(*attempt, args.chronicle, args.story, args.id);
                break;
            case AdminAction::DestroyChronicle:
                reply = call.timed(*attempt, args.chronicle);
                break;
            default:
                reply = call.timed(*attempt);
                break;
        }
        // Scalar replies have no remote access to caller memory. Native RPC
        // registration belongs to the engine and is never deregistered per call.
        return {reply, true, reply};
    }
    int shutdown_collection(std::shared_ptr<KeeperRegistry> registry, rpc::RpcDeadline const& deadline);

private:
    friend class AdminWork;
    DataStoreAdminClient(std::shared_ptr<VisorAdminRuntime> owner, std::string address, uint16_t provider)
        : owner_(std::move(owner))
        , address_(std::move(address))
        , provider_(provider)
    {}
    std::shared_ptr<VisorAdminRuntime> owner_;
    std::string address_;
    uint16_t provider_;
};
} // namespace chronolog
#endif
