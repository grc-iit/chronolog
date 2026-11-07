#ifndef CHRONOLOG_LIFECYCLE_RECORD_H
#define CHRONOLOG_LIFECYCLE_RECORD_H
#include <RpcRuntime.h>
#include <algorithm>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
namespace chronolog::rpc
{
enum class ShutdownState
{
    Running,
    Quiescing,
    Draining,
    Stopped
};
// Retained std-only state. Neither a destructor nor a caller starts native work.
struct LifecycleRecord
{
    std::atomic<ShutdownState> state{ShutdownState::Running};
    std::atomic<uint64_t> attempts{0};
    std::atomic<int> status{CL_SUCCESS};
    void quiesce() noexcept
    {
        auto previous = ShutdownState::Running;
        state.compare_exchange_strong(previous, ShutdownState::Quiescing, std::memory_order_acq_rel);
    }
    void draining() noexcept
    {
        state.store(ShutdownState::Draining, std::memory_order_release);
        status.store(CL_ERR_SHUTDOWN_INCOMPLETE, std::memory_order_release);
    }
    void stopped() noexcept
    {
        status.store(CL_SUCCESS, std::memory_order_release);
        state.store(ShutdownState::Stopped, std::memory_order_release);
    }
};
// One physical join, owned by an already admitted worker. Other admitted
// shutdown responses may observe it, but never join the same ES twice.
class JoinRecord: public std::enable_shared_from_this<JoinRecord>
{
public:
    enum class Phase
    {
        Ready,
        Joining,
        Complete,
        Unsafe
    };
    bool complete() const noexcept { return phase_.load(std::memory_order_acquire) == Phase::Complete; }
    bool unsafe() const noexcept { return phase_.load(std::memory_order_acquire) == Phase::Unsafe; }
    // Worker only; the exact first join may continue beyond a caller expiry.
    void finish(std::function<void()> const& join)
    {
        auto expected = Phase::Ready;
        if(phase_.compare_exchange_strong(expected, Phase::Joining, std::memory_order_acq_rel))
        {
            try
            {
                join();
                phase_.store(Phase::Complete, std::memory_order_release);
            }
            catch(...)
            {
                phase_.store(Phase::Unsafe, std::memory_order_release);
                throw;
            }
        }
        else
        {
            while(phase_.load(std::memory_order_acquire) == Phase::Joining)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if(!complete())
                throw std::runtime_error("Collection join did not establish completion");
        }
    }
    int start(RpcRuntime& runtime, RpcDeadline const& deadline, std::shared_ptr<void> owner, std::function<void()> join)
    {
        if(complete())
            return CL_SUCCESS;
        if(unsafe())
            return CL_ERR_SHUTDOWN_INCOMPLETE;
        if(scheduled_.load(std::memory_order_acquire))
            return CL_SUCCESS;
        if(!owner)
            return CL_ERR_INVALID_ARG;
        auto reservation = runtime.reserve(deadline);
        if(!reservation)
            return reservation.status();
        bool expected = false;
        if(!scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return CL_SUCCESS;
        class Work final: public OwnedRpcWork<int>
        {
            std::shared_ptr<JoinRecord> record_;
            std::shared_ptr<void> owner_;
            std::function<void()> join_;
            bool entered_ = false;

        public:
            Work(std::shared_ptr<JoinRecord> record, std::shared_ptr<void> owner, std::function<void()> join)
                : record_(std::move(record))
                , owner_(std::move(owner))
                , join_(std::move(join))
            {}
            ~Work() override
            {
                // A queued job abandoned before run has not joined anything.
                // Permit a later bounded caller to admit the same preparation.
                if(!entered_)
                    record_->scheduled_.store(false, std::memory_order_release);
            }
            RpcResult<int> run(OperationContext&) override
            {
                entered_ = true;
                record_->finish(join_);
                return {CL_SUCCESS, false, 1};
            }
        };
        try
        {
            runtime.submit<int>(std::move(reservation),
                                std::make_shared<Work>(shared_from_this(), std::move(owner), std::move(join)))
                    .releaseToRuntime();
        }
        catch(...)
        {
            scheduled_.store(false, std::memory_order_release);
            throw;
        }
        return CL_SUCCESS;
    }
    int wait(RpcDeadline const& deadline) const noexcept
    {
        while(!complete() && !unsafe() && !deadline.expired())
        {
            auto remaining = deadline.remaining();
            auto slice =
                    std::min(remaining,
                             std::chrono::duration_cast<RpcDeadline::Clock::duration>(std::chrono::milliseconds(1)));
            if(slice > RpcDeadline::Clock::duration::zero())
                std::this_thread::sleep_for(slice);
        }
        return complete() ? CL_SUCCESS : CL_ERR_SHUTDOWN_INCOMPLETE;
    }

private:
    std::atomic<Phase> phase_{Phase::Ready};
    std::atomic<bool> scheduled_{false};
};
} // namespace chronolog::rpc
#endif
