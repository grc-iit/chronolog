#ifndef CHRONOLOG_RPC_BULK_CONTROL_H
#define CHRONOLOG_RPC_BULK_CONTROL_H
#include <private/RpcBulkOwner.h>
#include <thread>
namespace chronolog::rpc
{
inline void bulkReady(std::shared_ptr<RpcBulkOwner> const& owner, thallium::request const& request) noexcept
{
    if(!owner || owner->closing)
        return;
    auto runtime = owner->runtime.lock();
    if(!runtime)
        return;
    auto reservation = runtime->reserve(RpcDeadline::after(std::chrono::milliseconds(owner->policy.attempt_ms)));
    if(!reservation)
        return;
    class Ready final: public OwnedRpcWork<int>
    {
        std::shared_ptr<RpcBulkOwner> owner_;
        thallium::request request_;

    public:
        Ready(std::shared_ptr<RpcBulkOwner> owner, thallium::request const& request)
            : owner_(std::move(owner))
            , request_(request)
        {}
        RpcResult<int> run(OperationContext& operation) override
        {
            if(!operation.tryBeginDispatch())
                return {CL_ERR_DEADLINE_EXCEEDED, false, 0};
            try
            {
                request_.respond(true);
                return {CL_SUCCESS, true, 1};
            }
            catch(...)
            {
                return {CL_ERR_OUTCOME_UNCONFIRMED, true, {}};
            }
        }
    };
    try
    {
        runtime->submit<int>(std::move(reservation), std::make_shared<Ready>(owner, request)).releaseToRuntime();
    }
    catch(...)
    {}
}
// This scope fence intentionally cannot claim B12 native retirement. Every
// referenced main-stack queue/store and provider stays alive on incomplete stop.
inline void retainBulkDependencies(std::shared_ptr<RpcBulkOwner> const& owner,
                                   std::shared_ptr<RpcRuntime> const& runtime)
{
    if(!owner)
        return;
    owner->quiesce();
    while(runtime->drain(RpcDeadline::after(std::chrono::milliseconds(owner->policy.shutdown_ms))) != CL_SUCCESS)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}
class BulkScopeFence
{
    std::shared_ptr<RpcBulkOwner> const& owner_;
    std::shared_ptr<RpcRuntime> const& runtime_;

public:
    BulkScopeFence(std::shared_ptr<RpcBulkOwner> const& owner, std::shared_ptr<RpcRuntime> const& runtime)
        : owner_(owner)
        , runtime_(runtime)
    {}
    ~BulkScopeFence() { retainBulkDependencies(owner_, runtime_); }
};
} // namespace chronolog::rpc
#endif
