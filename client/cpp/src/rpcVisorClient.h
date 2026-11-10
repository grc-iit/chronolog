#ifndef RPC_VISOR_PORTAL_CLIENT_H
#define RPC_VISOR_PORTAL_CLIENT_H
#include "ClientRpcContext.h"
#include <private/RpcCallState.h>
#include <AcquireStoryResponseMsg.h>
#include <ConnectResponseMsg.h>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/pair.hpp>
namespace chronolog
{
// Immutable peer descriptor. Lookup and invocation happen only in admitted work.
class RpcVisorClient
{
public:
    RpcVisorClient(thallium::engine& engine,
                   std::string address,
                   uint16_t provider,
                   std::shared_ptr<ClientRpcProcedures> procedures)
        : engine_(engine)
        , address_(std::move(address))
        , provider_(provider)
        , procedures_(std::move(procedures))
    {}
    template <class Reply, class... Args>
    rpc::RpcResult<Reply> call(ClientRpcProcedures::Name name,
                               rpc::OperationContext& context,
                               uint64_t attemptMs,
                               rpc::RpcCallState& native,
                               Args const&... args)
    {
        try
        {
            auto binding = procedures_->provision(name, provider_, context.deadline());
            if(binding != CL_SUCCESS)
                return {binding, false, std::nullopt};
            native.bind(engine_, procedures_->calls[name], address_, provider_);
            auto timeout = context.deadline().cappedAttempt(std::chrono::milliseconds(attemptMs));
            if(!timeout || !context.tryBeginDispatch())
                return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
            try
            {
                rpc::SettledOnExit settled{context};
                Reply reply = native.callable().timed(*timeout, args...);
                native.clearKnown();
                return {CL_SUCCESS, true, std::move(reply)};
            }
            catch(...)
            {
                return {CL_ERR_OUTCOME_UNCONFIRMED, true, std::nullopt};
            }
        }
        catch(...)
        {
            return {context.deadline().expired() ? CL_ERR_DEADLINE_EXCEEDED : CL_ERR_UNKNOWN, false, std::nullopt};
        }
    }

private:
    thallium::engine& engine_;
    std::string const address_;
    uint16_t const provider_;
    std::shared_ptr<ClientRpcProcedures> const procedures_;
};
} // namespace chronolog
#endif
