#ifndef CHRONOLOG_RPC_BULK_SEND_H
#define CHRONOLOG_RPC_BULK_SEND_H
#include <RpcRuntime.h>
#include <chronolog_errcode.h>
#include <ChunkReceipt.h>
#include <ServiceId.h>
#include <thallium/serialization/stl/string.hpp>
#include <private/RpcCallState.h>
#include <private/RpcBulkLifetime.h>
#include <functional>
#include <string>
namespace chronolog::rpc
{
template <class Reply>
struct BulkReplyState
{
    std::atomic<bool> known{false};
    int status = CL_ERR_OUTCOME_UNCONFIRMED;
    Reply reply{};
};
// One attempt. Untimed native receipt forwarding is intentional: only its
// decoded reply can establish that the peer will never access the source again.
// The runtime's independent waiter expires; this native continuation does not.
template <class Reply>
class BulkSendWork final: public OwnedRpcWork<Reply>
{
public:
    BulkSendWork(std::shared_ptr<void> owner,
                 thallium::engine& engine,
                 thallium::remote_procedure remote,
                 std::function<int(RpcDeadline const&)> provision,
                 ServiceId route,
                 std::string payload,
                 BulkPermits::Permit permit,
                 ServiceId reporter = {},
                 std::shared_ptr<BulkReplyState<Reply>> state = {},
                 std::function<std::string()> prepare = {},
                 std::function<void()> afterKnown = {})
        : owner_(std::move(owner))
        , engine_(engine)
        , remote_(std::move(remote))
        , provision_(std::move(provision))
        , route_(std::move(route))
        , permit_(std::move(permit))
        , prepare_(std::move(prepare))
        , payload_(std::move(payload))
        , reporter_(std::move(reporter))
        , state_(std::move(state))
        , afterKnown_(std::move(afterKnown))
    {}
    ~BulkSendWork() override
    {
        // Reserved submission can expire without run(). Cleanup is worker-
        // owned too, and must resolve that definitely undispatched outcome.
        if(state_ && !dispatched_ && !state_->known.load(std::memory_order_acquire))
        {
            state_->status = CL_ERR_DEADLINE_EXCEEDED;
            state_->known.store(true, std::memory_order_release);
        }
        if(afterKnown_)
            afterKnown_();
    }
    RpcResult<Reply> run(OperationContext& operation) override
    {
        try
        {
            if(prepare_)
            {
                payload_ = prepare_();
                prepare_ = {};
            }
            if(payload_.empty())
                return knownFailure(CL_ERR_INVALID_ARG);
            if(operation.deadline().expired())
                return knownFailure(CL_ERR_DEADLINE_EXCEEDED);
            int status = provision_(operation.deadline());
            if(status != CL_SUCCESS)
                return knownFailure(status);
            std::string address;
            route_.get_service_as_string(address);
            call_.bind(engine_, remote_, address, route_.getProviderId());
            if(operation.deadline().expired())
                return knownFailure(CL_ERR_DEADLINE_EXCEEDED);
            bulk_.emplace(engine_.expose({{payload_.data(), payload_.size()}}, thallium::bulk_mode::read_only));
            if(!operation.tryBeginDispatch())
                return knownFailure(CL_ERR_DEADLINE_EXCEEDED);
            dispatched_ = true;
            Reply reply;
            if constexpr(std::is_same_v<Reply, ChunkReceipt>)
                reply = call_.callable()(*bulk_, reporter_);
            else
                reply = call_.callable()(*bulk_);
            // Every peer branch replying here must finish all source access
            // before sending its existing scalar/ChunkReceipt wire response.
            auto bytes = [&]
            {
                if constexpr(std::is_same_v<Reply, ChunkReceipt>)
                    return reply.bytes;
                else
                    return reply;
            }();
            int result = bytes == payload_.size() ? static_cast<int>(CL_SUCCESS)
                                                  : static_cast<int>(CL_ERR_STORY_CHUNK_EXTRACTION);
            call_.clearKnown();
            bulk_.reset();
            std::string{}.swap(payload_);
            if(state_)
            {
                state_->reply = reply;
                state_->status = result;
                state_->known.store(true, std::memory_order_release);
            }
            return {result, true, std::move(reply)};
        }
        catch(...)
        {
            if(!dispatched_)
                return knownFailure(CL_ERR_UNKNOWN);
            return {CL_ERR_OUTCOME_UNCONFIRMED, true, std::nullopt};
        }
    }

private:
    RpcResult<Reply> knownFailure(int status)
    {
        if(state_)
        {
            state_->status = status;
            state_->known.store(true, std::memory_order_release);
        }
        return {status, false, std::nullopt};
    }
    std::shared_ptr<void> owner_;
    thallium::engine& engine_;
    thallium::remote_procedure remote_;
    std::function<int(RpcDeadline const&)> provision_;
    ServiceId route_;
    BulkPermits::Permit permit_;
    std::function<std::string()> prepare_;
    std::string payload_;
    ServiceId reporter_;
    std::shared_ptr<BulkReplyState<Reply>> state_;
    // Native state dies before source storage, descriptor or actual engine owner.
    std::function<void()> afterKnown_;
    bool dispatched_ = false;
    RpcCallState call_;
    std::optional<thallium::bulk> bulk_;
};
} // namespace chronolog::rpc
#endif
