#include <QueryResponseTransferAgent.h>
#include <ReplayEventMerge.h>
#include <private/RpcBulkSerialization.h>
namespace chronolog
{
int QueryResponseAgent::stashOwnedQueryResponse(ClientQueryId query,
                                                std::shared_ptr<PlaybackQueryResponse> const& response,
                                                bool ready)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queries_.emplace(query, Record{ready, response}).second ? CL_SUCCESS : CL_ERR_RPC_CAPACITY;
}
std::shared_ptr<PlaybackQueryResponse> QueryResponseAgent::takeReady(ClientQueryId query)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = queries_.find(query);
    if(found == queries_.end() || !found->second.ready)
        return {};
    auto response = std::move(found->second.response);
    queries_.erase(found);
    return response;
}
int QueryResponseAgent::addArchivedEventsToQueryResponse(ClientQueryId const& query,
                                                         std::list<StoryChunk*> const& chunks,
                                                         int status)
{
    std::vector<Event> archived;
    for(auto* chunk: chunks) chunk->extractEventSeries(archived);
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = queries_.find(query);
    if(found == queries_.end())
        return CL_ERR_UNKNOWN;
    auto& response = *found->second.response;
    if(status != CL_SUCCESS)
        response.complete = false;
    if(!archived.empty())
        response.events = mergeReplayEvents(std::move(archived), std::move(response.events));
    found->second.ready = true;
    return CL_SUCCESS;
}
rpc::RpcResult<uint64_t>
QueryResponseAgent::transferOwnedQueryResponse(std::shared_ptr<PlaybackQueryResponse const> response,
                                               rpc::RpcReservation reservation,
                                               rpc::BulkPermits::Permit permit,
                                               rpc::RpcDeadline const& replay)
{
    auto owner = owner_.lock();
    auto runtime = owner ? owner->runtime.lock() : nullptr;
    if(!owner || !runtime)
        return {CL_ERR_RPC_CAPACITY, false, {}};
    auto phaseStarted = rpc::RpcDeadline::Clock::now();
    auto total = rpc::RpcDeadline::fromStart(phaseStarted, std::chrono::milliseconds(owner->policy.transfer_ms));
    if(replay.expiresAt() < total.expiresAt())
        total = replay;
    auto attempt = rpc::RpcDeadline::fromStart(phaseStarted, std::chrono::milliseconds(owner->policy.attempt_ms));
    reservation.narrowDeadline(total);
    reservation.narrowDeadline(attempt);
    try
    {
        auto prepare = [response = std::move(response)] { return rpc::serializeBulk(*response); };
        auto route = route_;
        auto work = std::make_shared<rpc::BulkSendWork<uint64_t>>(
                owner,
                *owner->engine,
                owner->receiveResponse,
                [owner, provider = route.getProviderId()](rpc::RpcDeadline const& deadline)
                { return owner->provisionResponse(provider, deadline); },
                route,
                std::string{},
                std::move(permit),
                ServiceId{},
                std::shared_ptr<rpc::BulkReplyState<uint64_t>>{},
                std::move(prepare));
        return runtime->invoke<uint64_t>(std::move(reservation), std::move(work));
    }
    catch(...)
    {
        return {CL_ERR_UNKNOWN, false, {}};
    }
}
int QueryResponseAgent::transferQueryResponseToClient(PlaybackQueryResponse const& response)
{
    auto started = rpc::RpcDeadline::Clock::now();
    auto owner = owner_.lock();
    auto runtime = owner ? owner->runtime.lock() : nullptr;
    if(!owner || !runtime)
        return CL_ERR_RPC_CAPACITY;
    auto total = rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(owner->policy.transfer_ms));
    auto reservation = runtime->reserve(total);
    if(!reservation)
        return reservation.status();
    auto permit = owner->responseSources.acquire();
    if(!permit)
        return CL_ERR_RPC_CAPACITY;
    // Legacy borrowed input needs one admitted snapshot; the production
    // coordinator passes its already-owned response without another copy.
    auto snapshot = std::make_shared<PlaybackQueryResponse const>(response);
    return transferOwnedQueryResponse(std::move(snapshot), std::move(reservation), std::move(*permit), total).status;
}
} // namespace chronolog
