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
    rpc::RpcResult<int> execute(rpc::OperationContext& context, AdminArguments const& args, rpc::RpcCallState& native)
    {
        auto binding = owner_->provision(args.action, provider_, context.deadline());
        if(binding != CL_SUCCESS)
            return {binding, false, std::nullopt};
        auto endpoint = owner_->lookup->resolve(*owner_->engine, address_);
        auto remote = owner_->procedures[static_cast<std::size_t>(args.action)];
        native.bind(remote, endpoint, provider_);
        auto attempt = context.deadline().cappedAttempt(std::chrono::milliseconds(owner_->policy.attempt_ms));
        if(!attempt || !context.tryBeginDispatch())
            return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
        int reply;
        switch(args.action)
        {
            case AdminAction::Start:
                reply = native.callable().timed(*attempt, args.chronicle, args.story, args.id, args.start);
                break;
            case AdminAction::StartPlayer:
                reply = native.callable()
                                .timed(*attempt, args.chronicle, args.story, args.id, args.start, args.keepers);
                break;
            case AdminAction::Stop:
                reply = native.callable().timed(*attempt, args.id);
                break;
            case AdminAction::DestroyStory:
                reply = native.callable().timed(*attempt, args.chronicle, args.story, args.id);
                break;
            case AdminAction::DestroyChronicle:
                reply = native.callable().timed(*attempt, args.chronicle);
                break;
            case AdminAction::Shutdown:
                // The collector's real join can outlast one caller. Keep this
                // original native continuation charged until its actual ACK.
                reply = native.callable()();
                break;
            default:
                reply = native.callable().timed(*attempt);
                break;
        }
        native.clearKnown();
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
    std::timed_mutex shutdownMutex_;
    std::shared_ptr<AdminCompletion> shutdown_;
};
} // namespace chronolog
#endif
