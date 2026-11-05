#include "PlaybackQueryRpcClient.h"
#include "ClientQueryService.h"
#include <thallium/serialization/stl/string.hpp>
namespace chronolog
{
rpc::RpcResult<uint32_t> PlaybackQueryRpcClient::send_story_playback_request(ClientRpcContext& owner,
                                                                             rpc::OperationContext& operation,
                                                                             rpc::RpcCallState& native,
                                                                             ServiceId const& receiver,
                                                                             PlaybackQuery const& query,
                                                                             uint64_t attemptMs) const
{
    try
    {
        auto status =
                owner.procedures->provision(ClientRpcProcedures::PlaybackRequest, provider_, operation.deadline());
        if(status != CL_SUCCESS)
            return {status, false, {}};
        native.bind(*owner.engine, owner.procedures->calls[ClientRpcProcedures::PlaybackRequest], address_, provider_);
        auto timeout = operation.deadline().cappedAttempt(std::chrono::milliseconds(attemptMs));
        if(!timeout || !operation.tryBeginDispatch())
            return {CL_ERR_DEADLINE_EXCEEDED, false, {}};
        try
        {
            uint32_t reply = native.callable().timed(*timeout,
                                                     receiver,
                                                     query.queryId,
                                                     query.storyId,
                                                     query.chronicleName,
                                                     query.storyName,
                                                     query.startTime,
                                                     query.endTime);
            native.clearKnown();
            return {CL_SUCCESS, true, reply};
        }
        catch(...)
        {
            return {CL_ERR_OUTCOME_UNCONFIRMED, true, {}};
        }
    }
    catch(...)
    {
        return {operation.deadline().expired() ? CL_ERR_DEADLINE_EXCEEDED : CL_ERR_UNKNOWN, false, {}};
    }
}
} // namespace chronolog
