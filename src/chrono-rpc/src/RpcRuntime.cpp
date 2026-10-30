#include <RpcRuntime.h>
#include <thallium/timeout.hpp>
#include <mutex>
#include <pthread.h>
#include <system_error>
#include <thread>
#include <vector>

namespace chronolog::rpc
{
namespace
{
constexpr std::size_t DefaultCapacity = 2;
constexpr unsigned Closed = 1u << 8;
constexpr unsigned CountMask = Closed - 1;
constexpr auto PollInterval = std::chrono::milliseconds(1);

// No shared lock, native cleanup or diagnostic I/O on waiting caller paths.
void pause(RpcDeadline const& deadline) noexcept
{
    auto remaining = deadline.remaining();
    auto interval = std::chrono::duration_cast<RpcDeadline::Clock::duration>(PollInterval);
    if(remaining > RpcDeadline::Clock::duration::zero())
        std::this_thread::sleep_for(remaining < interval ? remaining : interval);
}
} // namespace

bool OperationContext::tryBeginDispatch() noexcept
{
    if(deadline_.expired())
    {
        abandon();
        return false;
    }
    auto observed = flags_.load(std::memory_order_acquire);
    while(!(observed & (Abandoned | Completed)))
    {
        if(flags_.compare_exchange_strong(observed, observed | Dispatched, std::memory_order_acq_rel))
            return true;
    }
    return false;
}

void OperationContext::abandon() noexcept
{
    auto observed = flags_.load(std::memory_order_acquire);
    while(!(observed & (Abandoned | Completed)))
    {
        if(flags_.compare_exchange_strong(observed, observed | Abandoned, std::memory_order_acq_rel))
            return;
    }
}

bool OperationContext::complete() noexcept
{
    if(deadline_.expired())
        abandon();
    auto observed = flags_.load(std::memory_order_acquire);
    while(!(observed & (Abandoned | Completed)))
    {
        if(flags_.compare_exchange_strong(observed, observed | Completed, std::memory_order_acq_rel))
            return true;
    }
    return false;
}

namespace detail
{
bool JobBase::wait() noexcept
{
    for(;;)
    {
        if(context.completed())
            return true;
        if(context.abandoned())
            return false;
        if(context.deadline().expired())
            context.abandon();
        else
            pause(context.deadline());
    }
}

void JobBase::abandonCaller() noexcept
{
    context.abandon();
    callerDone.store(true, std::memory_order_release);
}

RpcResult<int> JobBase::expiredResult() const noexcept
{
    bool dispatched = context.dispatched();
    return {dispatched ? CL_ERR_OUTCOME_UNCONFIRMED : CL_ERR_DEADLINE_EXCEEDED, dispatched, std::nullopt};
}

enum class Phase
{
    Empty,
    Reserved,
    Queued,
    Running,
    Retiring,
    Quarantined
};
struct Slot
{
    std::atomic<Phase> phase{Phase::Empty};
    std::shared_ptr<JobBase> job;
    std::shared_ptr<void> cleanup;
};

struct RuntimeState
{
    explicit RuntimeState(std::size_t count)
        : capacity(count)
        , slots(count)
        , workers(count)
        , exited(std::make_unique<std::atomic<bool>[]>(count))
        , joined(count, false)
        , arguments(count)
    {
        for(std::size_t i = 0; i != capacity; ++i) exited[i].store(false, std::memory_order_relaxed);
    }
    std::size_t const capacity;
    std::vector<Slot> slots;
    std::atomic<unsigned> admissions{0};
    std::atomic<bool> admissionStarted{false};
    std::vector<pthread_t> workers;
    std::unique_ptr<std::atomic<bool>[]> exited;
    std::vector<bool> joined;
    std::size_t started = 0;
    std::atomic<bool> starting{true};
    std::atomic<bool> reaped{false};
    struct WorkerArgument
    {
        RuntimeState* owner;
        std::size_t index;
    };
    std::vector<WorkerArgument> arguments;
    std::shared_ptr<void> retainedOwner;
    std::function<void()> ownerCleanup;
    enum class OwnerPhase
    {
        Unattached,
        Attached,
        Cleaning,
        Complete,
        Quarantined
    };
    std::atomic<OwnerPhase> ownerPhase{OwnerPhase::Unattached};

    void queue(std::size_t index, std::shared_ptr<JobBase> job) noexcept
    {
        if(job && (admissions.load(std::memory_order_acquire) & Closed))
            job->context.abandon();
        slots[index].job = std::move(job);
        slots[index].phase.store(Phase::Queued, std::memory_order_release);
    }

    void close() noexcept { admissions.fetch_or(Closed, std::memory_order_acq_rel); }

    void release(Slot& slot) noexcept
    {
        // All result/work/native destruction precedes both capacity release and
        // the worker-exit condition. No successor sees retained native owners.
        slot.job.reset();
        slot.cleanup.reset();
        slot.phase.store(Phase::Empty, std::memory_order_release);
        admissions.fetch_sub(1, std::memory_order_acq_rel);
    }

    bool process(Slot& slot)
    {
        auto phase = slot.phase.load(std::memory_order_acquire);
        if(phase == Phase::Retiring)
        {
            if(!slot.phase.compare_exchange_strong(phase, Phase::Running, std::memory_order_acq_rel))
                return false;
            if(!slot.job->callerDone.load(std::memory_order_acquire))
            {
                slot.phase.store(Phase::Retiring, std::memory_order_release);
                return false;
            }
            release(slot);
            return true;
        }
        if(phase != Phase::Queued ||
           !slot.phase.compare_exchange_strong(phase, Phase::Running, std::memory_order_acq_rel))
            return false;
        auto job = slot.job;
        slot.cleanup.reset(); // result allocation failure cleanup also runs here
        if(!job)
        {
            release(slot);
            return true;
        }
        if(job->context.deadline().expired())
            job->context.abandon();
        if(job->context.abandoned())
            job->fail(CL_ERR_DEADLINE_EXCEEDED);
        else
        {
            try
            {
                job->run();
            }
            catch(thallium::timeout const&)
            {
                // This type derives from std::exception, not thallium::exception.
                job->fail(CL_ERR_DEADLINE_EXCEEDED);
            }
            catch(...)
            {
                job->fail(CL_ERR_UNKNOWN);
            }
        }
        job->normalize();
        bool quarantine = !job->hasReply() && job->context.dispatched();
        job->context.complete();
        if(!quarantine)
            job->releaseWork();
        // Drop local ownership before another worker can reclaim the slot.
        job.reset();
        slot.phase.store(quarantine ? Phase::Quarantined : Phase::Retiring, std::memory_order_release);
        return true;
    }

    bool cleanupOwner()
    {
        auto phase = ownerPhase.load(std::memory_order_acquire);
        if(phase == OwnerPhase::Unattached || phase == OwnerPhase::Complete)
            return true;
        if(phase != OwnerPhase::Attached)
            return false;
        // Closed excludes new reservations. Count zero proves every normal
        // slot, work/result destructor and registration cleanup has settled.
        // Count is claimed BEFORE publishing Cleaning; no worker may exit
        // merely because it observed zero while this claim was in flight.
        unsigned idle = Closed;
        if(!admissions.compare_exchange_strong(idle, Closed + 1, std::memory_order_acq_rel))
            return false;
        phase = OwnerPhase::Attached;
        if(!ownerPhase.compare_exchange_strong(phase, OwnerPhase::Cleaning, std::memory_order_acq_rel))
        {
            admissions.fetch_sub(1, std::memory_order_acq_rel);
            return phase == OwnerPhase::Complete;
        }
        auto& slot = slots.front();
        slot.phase.store(Phase::Running, std::memory_order_release);
        try
        {
            ownerCleanup();
        }
        catch(...)
        {
            ownerPhase.store(OwnerPhase::Quarantined, std::memory_order_release);
            slot.phase.store(Phase::Quarantined, std::memory_order_release);
            return false;
        }
        // Drop every native-bearing capture on THIS charged worker. The
        // process reaper will see std-only state after all workers exit.
        ownerCleanup = {};
        retainedOwner.reset();
        ownerPhase.store(OwnerPhase::Complete, std::memory_order_release);
        slot.phase.store(Phase::Empty, std::memory_order_release);
        admissions.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }

    void loop(std::size_t index)
    {
        for(;;)
        {
            bool progressed = false;
            for(auto& slot: slots) progressed |= process(slot);
            auto count = admissions.load(std::memory_order_acquire);
            if((count & Closed) && !(count & CountMask) && cleanupOwner())
                break;
            if(!progressed)
            {
                // No caller notification: condition-variable broadcast itself
                // can take an untimed internal lock. Only workers sleep here.
                std::this_thread::sleep_for(PollInterval);
            }
        }
        exited[index].store(true, std::memory_order_release);
    }

    static void* workerEntry(void* pointer) noexcept
    {
        auto* argument = static_cast<WorkerArgument*>(pointer);
        argument->owner->loop(argument->index);
        return nullptr;
    }

    // Only the process reaper calls this. tryjoin, unlike an exit flag followed
    // by join(), cannot block when a worker is descheduled just before returning.
    bool tryReap() noexcept
    {
        if(starting.load(std::memory_order_acquire) || admissions.load(std::memory_order_acquire) != Closed)
            return false;
        for(std::size_t i = 0; i != started; ++i)
        {
            if(!joined[i] && exited[i].load(std::memory_order_acquire))
            {
                if(pthread_tryjoin_np(workers[i], nullptr) == 0)
                    joined[i] = true;
            }
            if(!joined[i])
                return false;
        }
        reaped.store(true, std::memory_order_release);
        return true;
    }
};

// Process-lifetime ownership is deliberate. An ambiguous transfer has no safe
// forced-exit/reclamation primitive. Keeping this root alive prevents static
// destruction from finalizing its engines or destroying joinable live workers.
// There is exactly ONE process reaper, in addition to C fixed workers per owner.
class ProcessOwners
{
public:
    ProcessOwners()
        : reaper_([this] { loop(); })
    {}
    void add(std::shared_ptr<RuntimeState> const& owner)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        owners_.push_back(owner);
    }
    static ProcessOwners& instance()
    {
        static auto* owners = new ProcessOwners;
        return *owners;
    }

private:
    void loop()
    {
        for(;;)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for(auto position = owners_.begin(); position != owners_.end();)
                {
                    if((*position)->tryReap())
                        position = owners_.erase(position);
                    else
                        ++position;
                }
            }
            std::this_thread::sleep_for(PollInterval);
        }
    }
    std::mutex mutex_;
    std::vector<std::shared_ptr<RuntimeState>> owners_;
    std::thread reaper_;
};

void holdReservedWork(std::shared_ptr<RuntimeState> const& owner, std::size_t slot, std::shared_ptr<void> work) noexcept
{
    owner->slots[slot].cleanup = std::move(work);
}

std::shared_ptr<void> reservedWork(std::shared_ptr<RuntimeState> const& owner, std::size_t slot) noexcept
{
    return owner->slots[slot].cleanup;
}

void queueReservedWork(std::shared_ptr<RuntimeState> const& owner,
                       std::size_t slot,
                       std::shared_ptr<JobBase> job) noexcept
{
    owner->queue(slot, std::move(job));
}
} // namespace detail

RpcReservation::RpcReservation(std::shared_ptr<detail::RuntimeState> state,
                               std::size_t slot,
                               RpcDeadline deadline) noexcept
    : state_(std::move(state))
    , slot_(slot)
    , deadline_(deadline)
    , status_(CL_SUCCESS)
{}

RpcReservation::RpcReservation(RpcReservation&& other) noexcept
    : state_(std::move(other.state_))
    , slot_(other.slot_)
    , deadline_(other.deadline_)
    , status_(other.status_)
{
    other.status_ = CL_ERR_INVALID_ARG;
}

RpcReservation& RpcReservation::operator=(RpcReservation&& other) noexcept
{
    if(this != &other)
    {
        reset();
        state_ = std::move(other.state_);
        slot_ = other.slot_;
        deadline_ = other.deadline_;
        status_ = other.status_;
        other.status_ = CL_ERR_INVALID_ARG;
    }
    return *this;
}

RpcReservation::~RpcReservation() { reset(); }

void RpcReservation::reset() noexcept
{
    if(state_)
    {
        detail::queueReservedWork(state_, slot_, {});
        state_.reset();
    }
}

RpcRuntime::RpcRuntime()
    : RpcRuntime(DefaultCapacity)
{}

RpcRuntime::RpcRuntime(std::size_t capacity)
{
    // Validate before allocating state, creating the process reaper or workers.
    if(capacity == 0 || capacity > CountMask)
        throw std::invalid_argument("Invalid RPC runtime capacity");
    state_ = std::make_shared<detail::RuntimeState>(capacity);
    detail::ProcessOwners::instance().add(state_);
    for(std::size_t i = 0; i != state_->capacity; ++i)
    {
        state_->arguments[i] = {state_.get(), i};
        int error =
                pthread_create(&state_->workers[i], nullptr, detail::RuntimeState::workerEntry, &state_->arguments[i]);
        if(error)
        {
            state_->close();
            state_->starting.store(false, std::memory_order_release);
            throw std::system_error(error, std::generic_category(), "Cannot create RPC worker");
        }
        ++state_->started;
    }
    state_->starting.store(false, std::memory_order_release);
}

RpcRuntime::~RpcRuntime() { state_->close(); }

RpcReservation RpcRuntime::reserve(RpcDeadline const& deadline) noexcept
{
    state_->admissionStarted.store(true, std::memory_order_release);
    if(deadline.expired())
        return RpcReservation(CL_ERR_DEADLINE_EXCEEDED);
    auto observed = state_->admissions.load(std::memory_order_acquire);
    while(!(observed & Closed) && (observed & CountMask) < state_->capacity)
    {
        if(deadline.expired())
            return RpcReservation(CL_ERR_DEADLINE_EXCEEDED);
        if(state_->admissions.compare_exchange_weak(observed, observed + 1, std::memory_order_acq_rel))
        {
            for(std::size_t i = 0; i != state_->capacity; ++i)
            {
                auto phase = detail::Phase::Empty;
                if(state_->slots[i].phase.compare_exchange_strong(phase,
                                                                  detail::Phase::Reserved,
                                                                  std::memory_order_acq_rel))
                    return RpcReservation(state_, i, deadline);
            }
            // A completed release publishes Empty before decrementing the count.
            // A finite scan can still lose to a concurrent reservation; refuse.
            state_->admissions.fetch_sub(1, std::memory_order_acq_rel);
            return RpcReservation(CL_ERR_RPC_CAPACITY);
        }
    }
    return RpcReservation(CL_ERR_RPC_CAPACITY);
}

void RpcRuntime::retainOwner(std::shared_ptr<void> owner, std::function<void()> cleanup)
{
    if(!owner || !cleanup || state_->ownerPhase.load() != detail::RuntimeState::OwnerPhase::Unattached ||
       state_->admissions.load(std::memory_order_acquire) != 0 ||
       state_->admissionStarted.load(std::memory_order_acquire))
        throw std::logic_error("RPC owner must attach once before admission");
    state_->retainedOwner = std::move(owner);
    state_->ownerCleanup = std::move(cleanup);
    state_->ownerPhase.store(detail::RuntimeState::OwnerPhase::Attached, std::memory_order_release);
}

int RpcRuntime::drain(RpcDeadline const& deadline)
{
    state_->close();
    while(!state_->reaped.load(std::memory_order_acquire))
    {
        if(deadline.expired())
            return CL_ERR_SHUTDOWN_INCOMPLETE;
        pause(deadline);
    }
    return CL_SUCCESS;
}
} // namespace chronolog::rpc
