#ifndef CHRONOLOG_SERVICE_LIFECYCLE_H
#define CHRONOLOG_SERVICE_LIFECYCLE_H
#include <private/LifecycleStep.h>
#include <private/RpcBulkOwner.h>
namespace chronolog::rpc
{
inline int initializeService(std::shared_ptr<RpcBulkOwner> const& owner,
                             RpcRuntime& runtime,
                             NativeMidSpec spec,
                             RpcDeadline const& deadline,
                             std::shared_ptr<LifecycleStep>& step,
                             std::shared_ptr<void> root,
                             std::function<int()> setup = {},
                             volatile sig_atomic_t const* interrupt = nullptr)
{
    int status = startLifecycleStep(runtime,
                                    deadline,
                                    step,
                                    std::move(root),
                                    [owner, spec = std::move(spec), setup = std::move(setup)]() mutable
                                    {
                                        int status = owner->initialize(std::move(spec));
                                        return status == CL_SUCCESS && setup ? setup() : status;
                                    });
    return status == CL_SUCCESS ? waitLifecycleStep(step, deadline, interrupt) : status;
}
inline bool allControlIdle(RpcRuntime& runtime, std::size_t capacity, RpcDeadline const& deadline)
{
    while(!deadline.expired())
    {
        std::vector<RpcReservation> held;
        held.reserve(capacity);
        while(held.size() != capacity)
        {
            auto token = runtime.reserve(deadline);
            if(!token)
                break;
            held.push_back(std::move(token));
        }
        if(held.size() == capacity)
            return true;
        held.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
inline bool retireServices(std::vector<std::shared_ptr<RpcBulkOwner>> const& owners,
                           std::vector<std::shared_ptr<RpcRuntime>> const& runtimes,
                           RpcDeadline const& deadline)
{
    for(auto const& owner: owners)
    {
        owner->quiesce();
        owner->retirementReady.store(true, std::memory_order_release);
        if(!owner->consumeKnownOutcomes(deadline))
            return false;
    }
    for(auto const& runtime: runtimes)
        if(runtime->drain(deadline) != CL_SUCCESS)
            return false;
    return true;
}
} // namespace chronolog::rpc
#endif
