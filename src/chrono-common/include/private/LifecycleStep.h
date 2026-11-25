#ifndef CHRONOLOG_LIFECYCLE_STEP_H
#define CHRONOLOG_LIFECYCLE_STEP_H
#include <LifecycleRecord.h>
#include <csignal>
#include <cstdio>
namespace chronolog::rpc
{
// One retained local native action on an existing actual owner's worker.
// Caller expiry never destroys its callable or starts replacement work.
struct LifecycleStep
{
    std::atomic<bool> done{false};
    int status = CL_ERR_SHUTDOWN_INCOMPLETE;
    bool unsafe = false;
    bool begun = false;
};
inline int startLifecycleStep(RpcRuntime& runtime,
                              RpcDeadline const& deadline,
                              std::shared_ptr<LifecycleStep>& step,
                              std::shared_ptr<void> root,
                              std::function<int()> function)
{
    if(step && step->done.load(std::memory_order_acquire) && !step->begun && !step->unsafe)
        step.reset(); // Only a proved pre-action expiry permits readmission.
    if(step)
        return CL_SUCCESS;
    auto reservation = runtime.reserve(deadline);
    if(!reservation)
        return reservation.status();
    auto prepared = std::make_shared<LifecycleStep>();
    class Work final: public OwnedRpcWork<int>
    {
        std::shared_ptr<LifecycleStep> step_;
        std::shared_ptr<void> root_;
        std::function<int()> function_;
        bool entered_ = false;

    public:
        Work(std::shared_ptr<LifecycleStep> step, std::shared_ptr<void> root, std::function<int()> function)
            : step_(std::move(step))
            , root_(std::move(root))
            , function_(std::move(function))
        {}
        ~Work() override
        {
            if(!entered_)
            {
                step_->status = CL_ERR_DEADLINE_EXCEEDED;
                step_->done.store(true, std::memory_order_release);
            }
        }
        RpcResult<int> run(OperationContext& operation) override
        {
            entered_ = true;
            if(!operation.tryBeginDispatch())
            {
                step_->status = CL_ERR_DEADLINE_EXCEEDED;
                step_->done.store(true, std::memory_order_release);
                return {CL_SUCCESS, false, step_->status};
            }
            step_->begun = true;
            try
            {
                step_->status = function_();
            }
            catch(std::exception const& error)
            {
                std::fprintf(stderr, "Lifecycle native action failed: %s\n", error.what());
                step_->status = CL_ERR_OUTCOME_UNCONFIRMED;
                step_->unsafe = true;
            }
            catch(...)
            {
                std::fprintf(stderr, "Lifecycle native action failed with an unknown exception\n");
                // Retain an exceptional native action in its original slot.
                // No caller interprets this as successful settlement/cleanup.
                step_->status = CL_ERR_OUTCOME_UNCONFIRMED;
                step_->unsafe = true;
            }
            step_->done.store(true, std::memory_order_release);
            if(step_->unsafe)
                throw std::runtime_error("Lifecycle native action remains unsafe");
            return {CL_SUCCESS, false, step_->status};
        }
    };
    runtime.submit<int>(std::move(reservation), std::make_shared<Work>(prepared, std::move(root), std::move(function)))
            .releaseToRuntime();
    step = std::move(prepared);
    return CL_SUCCESS;
}
// Retry only refusal before a work item exists. Existing Running/unsafe work
// remains the same record/slot; caller attempts never replace it. The supplied
// expiry is unchanged, including time spent waiting for observation-token cleanup.
inline int startLifecycleStepWhenAvailable(RpcRuntime& runtime,
                                           RpcDeadline const& deadline,
                                           std::shared_ptr<LifecycleStep>& step,
                                           std::shared_ptr<void> root,
                                           std::function<int()> const& function)
{
    while(!deadline.expired())
    {
        int status = startLifecycleStep(runtime, deadline, step, root, function);
        if(status == CL_ERR_DEADLINE_EXCEEDED)
            return CL_ERR_SHUTDOWN_INCOMPLETE;
        if(status != CL_ERR_RPC_CAPACITY)
            return status;
        std::this_thread::sleep_for(
                std::min(deadline.remaining(),
                         std::chrono::duration_cast<RpcDeadline::Clock::duration>(std::chrono::milliseconds(1))));
    }
    return CL_ERR_SHUTDOWN_INCOMPLETE;
}
inline int waitLifecycleStep(std::shared_ptr<LifecycleStep> const& step,
                             RpcDeadline const& deadline,
                             volatile sig_atomic_t const* interrupt = nullptr)
{
    if(!step)
        return CL_ERR_SHUTDOWN_INCOMPLETE;
    while(!step->done.load(std::memory_order_acquire) && !deadline.expired() && (!interrupt || !*interrupt))
        std::this_thread::sleep_for(
                std::min(deadline.remaining(),
                         std::chrono::duration_cast<RpcDeadline::Clock::duration>(std::chrono::milliseconds(1))));
    return step->done.load(std::memory_order_acquire) ? step->status : CL_ERR_SHUTDOWN_INCOMPLETE;
}
} // namespace chronolog::rpc
#endif
