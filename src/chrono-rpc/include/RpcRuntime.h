#ifndef CHRONOLOG_RPC_RUNTIME_H
#define CHRONOLOG_RPC_RUNTIME_H

#include <RpcDeadline.h>
#include <client_errcode.h>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace chronolog::rpc
{
namespace detail
{
struct RuntimeState;
struct JobBase;
} // namespace detail

template <class Reply>
struct RpcResult
{
    int status;
    bool may_have_dispatched;
    std::optional<Reply> reply;
};

class OperationContext
{
public:
    RpcDeadline const& deadline() const noexcept { return deadline_; }
    // Reserve immediately before each native forward, after lookup/preparation.
    // A false return forbids forwarding. A winning reservation can conservatively
    // report dispatch even when the worker is descheduled before the native call.
    bool tryBeginDispatch() noexcept;

private:
    friend struct detail::JobBase;
    friend struct detail::RuntimeState;
    explicit OperationContext(RpcDeadline deadline) noexcept
        : deadline_(deadline)
    {}
    enum : unsigned
    {
        Abandoned = 1,
        Dispatched = 2,
        Completed = 4
    };
    void abandon() noexcept;
    bool complete() noexcept;
    bool dispatched() const noexcept { return (flags_.load(std::memory_order_acquire) & Dispatched) != 0; }
    bool abandoned() const noexcept { return (flags_.load(std::memory_order_acquire) & Abandoned) != 0; }
    bool completed() const noexcept { return (flags_.load(std::memory_order_acquire) & Completed) != 0; }
    RpcDeadline deadline_;
    std::atomic<unsigned> flags_{0};
};

// Trusted private adapters only. Own every argument, endpoint, engine lease and
// registration; never borrow caller storage. One native outbound request at a
// time per work item. Construct/lookup/finalize inside run when those may block.
// A reply asserts the adapter's no-future-access settlement boundary, including
// remote rejection. Reserved dispatch without a reply permanently retains this
// work and its slot. Local timeout/cancellation/error is not bulk settlement.
// Reply moves and destruction on the receiving caller must be nonblocking.
template <class Reply>
class OwnedRpcWork
{
public:
    virtual ~OwnedRpcWork() = default;
    virtual RpcResult<Reply> run(OperationContext&) = 0;
};

// Reserve BEFORE allocating/copying payloads or creating native registrations.
// Dropping a valid token only queues cleanup on an existing worker; the slot
// remains charged until that cleanup completes. A held token can prevent drain.
class RpcReservation
{
public:
    RpcReservation() noexcept = default;
    RpcReservation(RpcReservation const&) = delete;
    RpcReservation& operator=(RpcReservation const&) = delete;
    RpcReservation(RpcReservation&&) noexcept;
    RpcReservation& operator=(RpcReservation&&) noexcept;
    ~RpcReservation();
    int status() const noexcept { return status_; }
    explicit operator bool() const noexcept { return state_ != nullptr; }

private:
    friend class RpcRuntime;
    RpcReservation(std::shared_ptr<detail::RuntimeState> state, std::size_t slot, RpcDeadline deadline) noexcept;
    explicit RpcReservation(int status) noexcept
        : status_(status)
    {}
    void reset() noexcept;
    std::shared_ptr<detail::RuntimeState> state_;
    std::size_t slot_ = 0;
    std::optional<RpcDeadline> deadline_;
    int status_ = CL_ERR_INVALID_ARG;
};

namespace detail
{
// Published result storage has one writer. Completion/abandonment arbitration
// publishes it with release/acquire; callers never acquire a result/queue lock.
struct JobBase
{
    explicit JobBase(RpcDeadline deadline) noexcept
        : context(deadline)
    {}
    virtual ~JobBase() = default;
    virtual void run() = 0;
    virtual void fail(int status) noexcept = 0;
    virtual bool hasReply() const noexcept = 0;
    virtual void normalize() noexcept = 0;
    virtual void releaseWork() noexcept = 0;
    bool wait() noexcept;
    RpcResult<int> expiredResult() const noexcept;
    OperationContext context;
    std::atomic<bool> callerDone{false};
};

template <class Reply>
struct Job final: JobBase
{
    Job(RpcDeadline deadline, std::shared_ptr<OwnedRpcWork<Reply>> work) noexcept
        : JobBase(deadline)
        , work_(std::move(work))
    {}
    void run() override { result_.emplace(work_->run(context)); }
    void fail(int status) noexcept override { result_.emplace(RpcResult<Reply>{status, false, std::nullopt}); }
    bool hasReply() const noexcept override { return result_->reply.has_value(); }
    void normalize() noexcept override
    {
        auto state = expiredResult();
        result_->may_have_dispatched |= state.may_have_dispatched;
        if(!result_->reply && result_->may_have_dispatched)
            result_->status = CL_ERR_OUTCOME_UNCONFIRMED;
    }
    void releaseWork() noexcept override { work_.reset(); }
    std::shared_ptr<OwnedRpcWork<Reply>> work_;
    std::optional<RpcResult<Reply>> result_;
};

void holdReservedWork(std::shared_ptr<RuntimeState> const&, std::size_t, std::shared_ptr<void>) noexcept;
std::shared_ptr<void> reservedWork(std::shared_ptr<RuntimeState> const&, std::size_t) noexcept;
void queueReservedWork(std::shared_ptr<RuntimeState> const&, std::size_t, std::shared_ptr<JobBase>) noexcept;
} // namespace detail

// Create one runtime owner per real component/client engine context, before its
// bounded entries. Construction creates exactly two fixed joinable workers; it
// creates no engine. Do not replace a saturated runtime to bypass its cap.
// The process owner roots live states independently of this public handle.
// Dropping the handle requests quiescence, never joins/finalizes native work.
// One process-wide reaper collects fully settled/exited owners. Incomplete or
// quarantined owners stay rooted, possibly for the rest of the process lifetime.
class RpcRuntime
{
public:
    RpcRuntime();
    RpcRuntime(RpcRuntime const&) = delete;
    RpcRuntime& operator=(RpcRuntime const&) = delete;
    ~RpcRuntime();
    RpcReservation reserve(RpcDeadline const&) noexcept;

    // Convenience for lightweight descriptors with NONBLOCKING destruction, or
    // separately rooted resources. Rejection may drop this argument on caller.
    // Use reserve + the overload below when work destruction can block.
    template <class Reply>
    RpcResult<Reply> invoke(RpcDeadline const& deadline, std::shared_ptr<OwnedRpcWork<Reply>> work)
    {
        auto reservation = reserve(deadline);
        if(!reservation)
            return {reservation.status(), false, std::nullopt};
        return invoke<Reply>(std::move(reservation), std::move(work));
    }

    // Requires a valid token obtained from this owner. Ownership is queued even
    // if its original deadline or close already won. No fresh operation budget.
    // Prepare large/engine state in owned work; captures must own their storage.
    template <class Reply>
    RpcResult<Reply> invoke(RpcReservation&& reservation, std::shared_ptr<OwnedRpcWork<Reply>> work)
    {
        static_assert(std::is_nothrow_move_constructible_v<Reply>, "RPC reply must be nothrow movable");
        if(!reservation)
            return {CL_ERR_INVALID_ARG, false, std::nullopt};
        auto owner = reservation.state_;
        auto slot = reservation.slot_;
        auto deadline = *reservation.deadline_;
        // Transfer the last native owner BEFORE result allocation can fail.
        detail::holdReservedWork(owner, slot, std::move(work));
        std::shared_ptr<detail::Job<Reply>> job;
        try
        {
            auto owned = std::static_pointer_cast<OwnedRpcWork<Reply>>(detail::reservedWork(owner, slot));
            job = std::make_shared<detail::Job<Reply>>(deadline, std::move(owned));
        }
        catch(...)
        {
            detail::queueReservedWork(owner, slot, {});
            reservation.state_.reset();
            return {CL_ERR_RPC_CAPACITY, false, std::nullopt};
        }
        detail::queueReservedWork(owner, slot, job);
        reservation.state_.reset();
        if(!job->wait())
        {
            auto expired = job->expiredResult();
            job->callerDone.store(true, std::memory_order_release);
            return {expired.status, expired.may_have_dispatched, std::nullopt};
        }
        auto result = std::move(*job->result_);
        job->callerDone.store(true, std::memory_order_release);
        return result;
    }

    int drain(RpcDeadline const&);

private:
    std::shared_ptr<detail::RuntimeState> state_;
};
} // namespace chronolog::rpc
#endif
