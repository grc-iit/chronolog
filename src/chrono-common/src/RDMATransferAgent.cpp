#include <RDMATransferAgent.h>
#include <StoryChunk.h>
#include <private/RpcBulkSerialization.h>
#include <sstream>
#include <thread>
namespace chronolog
{
namespace
{
rpc::RpcDeadline attempt(std::shared_ptr<rpc::RpcBulkOwner> const& owner, rpc::RpcDeadline::Clock::time_point started)
{
    return rpc::RpcDeadline::fromStart(
            started,
            std::chrono::milliseconds(std::min(owner->policy.transfer_ms, owner->policy.attempt_ms)));
}
auto bulkWork(std::shared_ptr<rpc::RpcBulkOwner> owner,
              ServiceId route,
              std::string payload,
              rpc::BulkPermits::Permit permit,
              ServiceId reporter,
              std::shared_ptr<rpc::BulkReplyState<ChunkReceipt>> state = {},
              std::function<std::string()> prepare = {},
              std::function<void()> afterKnown = {})
{
    return std::make_shared<rpc::BulkSendWork<ChunkReceipt>>(
            owner,
            *owner->engine,
            owner->receiveChunk,
            [owner, provider = route.getProviderId()](rpc::RpcDeadline const& deadline)
            { return owner->provision("receive_story_chunk", provider, deadline); },
            std::move(route),
            std::move(payload),
            std::move(permit),
            std::move(reporter),
            std::move(state),
            std::move(prepare),
            std::move(afterKnown));
}
} // namespace
int RDMATransferAgent::transfer_serialized_story_chunk(std::string const& payload,
                                                       ServiceId const& reporter,
                                                       ChunkReceipt* receipt)
{
    auto started = rpc::RpcDeadline::Clock::now();
    auto owner = owner_.lock();
    if(!owner)
        return CL_ERR_RPC_CAPACITY;
    auto deadline = attempt(owner, started);
    if(payload.empty())
        return CL_ERR_INVALID_ARG;
    auto runtime = owner->runtime.lock();
    if(!runtime || owner->closing)
        return CL_ERR_RPC_CAPACITY;
    auto reservation = runtime->reserve(deadline);
    if(!reservation)
        return reservation.status();
    auto permit = owner->sources.acquire();
    if(!permit)
        return CL_ERR_RPC_CAPACITY;
    try
    {
        auto result = runtime->invoke<ChunkReceipt>(
                std::move(reservation),
                bulkWork(owner, route_, std::string(payload), std::move(*permit), reporter));
        if(result.status == CL_SUCCESS && result.reply && receipt)
            *receipt = *result.reply;
        return result.status;
    }
    catch(...)
    {
        return CL_ERR_UNKNOWN;
    }
}
int RDMATransferAgent::transfer_story_chunk(StoryChunk const& chunk, ServiceId const& reporter, ChunkReceipt* receipt)
{
    auto started = rpc::RpcDeadline::Clock::now();
    auto owner = owner_.lock();
    if(!owner)
        return CL_ERR_RPC_CAPACITY;
    auto deadline = attempt(owner, started);
    auto runtime = owner->runtime.lock();
    rpc::RpcBulkOwner::ChunkIdentity identity{route_.getProtocol(),
                                              route_.getIPaddr(),
                                              route_.getPort(),
                                              route_.getProviderId(),
                                              reporter.getProtocol(),
                                              reporter.getIPaddr(),
                                              reporter.getPort(),
                                              reporter.getProviderId(),
                                              chunk.getStoryId(),
                                              chunk.getStartTime(),
                                              chunk.getEndTime()};
    std::unique_lock<std::timed_mutex> lock(owner->pendingMutex, std::defer_lock);
    if(!lock.try_lock_until(deadline.expiresAt()))
        return CL_ERR_DEADLINE_EXCEEDED;
    // Only the originating caller's already-consumed result may be removed.
    // Another chunk cannot evict a late receipt that has not been collected.
    for(auto entry = owner->chunks.begin(); entry != owner->chunks.end();)
        if(entry->second->consumed.load(std::memory_order_acquire))
            entry = owner->chunks.erase(entry);
        else
            ++entry;
    if(deadline.expired())
        return CL_ERR_DEADLINE_EXCEEDED;
    auto matches = owner->chunks.equal_range(identity);
    for(auto found = matches.first; found != matches.second; ++found)
    {
        auto outcome = found->second;
        bool same = outcome->snapshot && outcome->snapshot->sameSerializedData(chunk);
        if(deadline.expired())
            return CL_ERR_DEADLINE_EXCEEDED;
        if(!same)
            continue;
        auto const& state = outcome->state;
        if(!state->known.load(std::memory_order_acquire))
            return CL_ERR_OUTCOME_UNCONFIRMED;
        int status = state->status;
        if(status == CL_SUCCESS && receipt)
            *receipt = state->reply;
        owner->chunks.erase(found);
        outcome->consumed.store(true, std::memory_order_release);
        return status;
    }
    if(!runtime || owner->closing)
        return CL_ERR_RPC_CAPACITY;
    auto reserved = runtime->reserve(deadline);
    if(!reserved)
        return reserved.status();
    auto source = owner->sources.acquire();
    if(!source)
        return CL_ERR_RPC_CAPACITY;
    auto metadata = owner->pendingOutcomes.acquire();
    if(!metadata)
        return CL_ERR_RPC_CAPACITY;
    std::shared_ptr<rpc::RpcBulkOwner::ChunkOutcome> outcome;
    try
    {
        outcome = std::make_shared<rpc::RpcBulkOwner::ChunkOutcome>(std::move(*metadata));
        owner->chunks.emplace(identity, outcome);
        // One admitted immutable snapshot of borrowed input. Serialization
        // happens on the charged worker, before its independent wait expires.
        outcome->snapshot = std::make_shared<StoryChunk const>(chunk);
        auto before = owner->beforeSerialize;
        auto prepare = [outcome, before]
        {
            if(before)
                before();
            return rpc::serializeBulk(*outcome->snapshot);
        };
        auto afterKnown = [owner, outcome, identity]
        {
            // A late receipt retains its immutable identity and charged slot
            // until the originating chunk consumes it. Large snapshot cleanup
            // stays on this existing worker, never the receipt collector.
            while(!outcome->consumed.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::lock_guard<std::timed_mutex> lock(owner->pendingMutex);
            outcome->snapshot.reset();
            auto range = owner->chunks.equal_range(identity);
            for(auto found = range.first; found != range.second; ++found)
                if(found->second == outcome)
                {
                    owner->chunks.erase(found);
                    break;
                }
        };
        auto work = bulkWork(owner,
                             route_,
                             {},
                             std::move(*source),
                             reporter,
                             outcome->state,
                             std::move(prepare),
                             std::move(afterKnown));
        lock.unlock();
        auto result = runtime->invoke<ChunkReceipt>(std::move(reserved), std::move(work));
        if(result.status == CL_SUCCESS && result.reply && receipt)
            *receipt = *result.reply;
        if(result.reply || !result.may_have_dispatched)
            outcome->consumed.store(true, std::memory_order_release);
        return result.status;
    }
    catch(...)
    {
        if(lock.owns_lock())
        {
            auto range = owner->chunks.equal_range(identity);
            for(auto found = range.first; found != range.second; ++found)
                if(found->second == outcome)
                {
                    owner->chunks.erase(found);
                    break;
                }
        }
        return CL_ERR_UNKNOWN;
    }
}
bool RDMATransferAgent::is_receiver_available() const
{
    auto owner = owner_.lock();
    if(!owner)
        return false;
    auto runtime = owner->runtime.lock();
    if(!runtime || owner->closing)
        return false;
    class Available: public rpc::OwnedRpcWork<bool>
    {
        std::shared_ptr<rpc::RpcBulkOwner> owner_;
        ServiceId route_;
        rpc::RpcCallState call_;

    public:
        Available(std::shared_ptr<rpc::RpcBulkOwner> owner, ServiceId route)
            : owner_(std::move(owner))
            , route_(std::move(route))
        {}
        rpc::RpcResult<bool> run(rpc::OperationContext& context) override
        {
            bool dispatched = false;
            try
            {
                auto status = owner_->provision("receiver_available", route_.getProviderId(), context.deadline());
                if(status != CL_SUCCESS)
                    return {status, false, {}};
                std::string address;
                route_.get_service_as_string(address);
                call_.bind(*owner_->engine, owner_->available, address, route_.getProviderId());
                auto timeout = context.deadline().cappedAttempt(std::chrono::milliseconds(owner_->policy.attempt_ms));
                if(!timeout || !context.tryBeginDispatch())
                    return {CL_ERR_DEADLINE_EXCEEDED, false, {}};
                dispatched = true;
                rpc::SettledOnExit settled{context};
                bool reply = call_.callable().timed(*timeout);
                call_.clearKnown();
                return {CL_SUCCESS, true, reply};
            }
            catch(...)
            {
                return {CL_ERR_OUTCOME_UNCONFIRMED, dispatched, {}};
            }
        }
    };
    auto result = runtime->invoke<bool>(rpc::RpcDeadline::after(std::chrono::milliseconds(owner->policy.attempt_ms)),
                                        std::make_shared<Available>(owner, route_));
    return result.reply.value_or(false);
}
} // namespace chronolog
