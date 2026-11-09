#ifndef CHRONOLOG_LIFECYCLE_PUBLISHER_H
#define CHRONOLOG_LIFECYCLE_PUBLISHER_H
#include <private/LifecycleStatus.h>
#include <private/LifecycleStep.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
namespace chronolog::rpc
{
// One status and diagnostic publisher per daemon. The coordinator thread
// never opens, writes, renames or logs. It replaces the wanted record and
// admits at most one retained LifecycleStep, on an existing runtime and under
// the caller's own deadline, to write it. A write held by a stalled file
// system or logger keeps that one step and its slot. Later requests replace
// the wanted record but admit nothing until the held step settles, and no
// step is admitted after the deadline it is given has expired.
//
// The Stopped record is written by publishTerminal() from the cleanup callback
// of the last original native owner, after its physical cleanup, so it carries
// the final native snapshot. A known local write failure there is retried on
// the same worker, with the same content, once per later stop attempt. A held
// write is never retried. The coordinator reports the daemon stopped only once
// terminalPublished() is true.
class LifecyclePublisher: public std::enable_shared_from_this<LifecyclePublisher>
{
public:
    using Clock = RpcDeadline::Clock;
    struct Record
    {
        ShutdownState state = ShutdownState::Running;
        int status = CL_SUCCESS;
        uint64_t attempt = 0;
        Clock::time_point entered{};
        std::optional<NativeRetirementSnapshot> native;
        std::size_t applicationHandles = 0;
        std::optional<LifecycleStatus::Startup> startup;
        // Runs on the publishing worker after the write, whether or not it
        // succeeded. Daemons put their warning log here, never on the caller.
        std::function<void()> diagnostic;
    };
    // Returns true only when the record is known to be in place.
    using Writer = std::function<bool(Record const&)>;
    static Record record(ShutdownState state,
                         int status,
                         uint64_t attempt,
                         Clock::time_point entered,
                         NativeRetirementSnapshot const& native,
                         std::size_t applicationHandles,
                         std::optional<LifecycleStatus::Startup> startup = std::nullopt,
                         std::function<void()> diagnostic = {})
    {
        return Record{state, status, attempt, entered, native, applicationHandles, startup, std::move(diagnostic)};
    }

    static std::shared_ptr<LifecyclePublisher> Create(std::string const& config)
    {
        auto status = std::make_shared<LifecycleStatus const>(config);
        return Create(
                [status](Record const& record)
                {
                    return status->publish(record.state,
                                           record.status,
                                           record.attempt,
                                           record.entered,
                                           record.native ? &*record.native : nullptr,
                                           record.applicationHandles,
                                           record.startup);
                });
    }
    static std::shared_ptr<LifecyclePublisher> Create(Writer writer)
    {
        return std::shared_ptr<LifecyclePublisher>(new LifecyclePublisher(std::move(writer)));
    }

    // Coordinator, at each explicit stop request. Never waits.
    void beginAttempt(uint64_t attempt, Clock::time_point entered) noexcept
    {
        version_.fetch_add(1, std::memory_order_acq_rel);
        entered_.store(entered.time_since_epoch().count(), std::memory_order_release);
        attempt_.store(attempt, std::memory_order_release);
        version_.fetch_add(1, std::memory_order_acq_rel);
    }

    // Coordinator. Replaces the wanted nonterminal record; does no I/O.
    uint64_t request(Record record)
    {
        auto wanted = std::make_shared<Wanted const>(Wanted{++requested_, std::move(record)});
        std::atomic_store_explicit(&wanted_, std::move(wanted), std::memory_order_release);
        return requested_;
    }

    // Coordinator. Admits one step for the wanted record when no step is in
    // progress and that record has not been written or failed yet. A refused
    // reservation leaves it wanted for a later call before the same deadline.
    void pump(RpcRuntime& runtime, RpcDeadline const& deadline, std::shared_ptr<void> root)
    {
        if(step_ && !step_->done.load(std::memory_order_acquire))
            return; // Running or held: observe it, never replace it.
        auto settled = std::max(written_.load(std::memory_order_acquire), failed_.load(std::memory_order_acquire));
        if(requested_ <= settled || deadline.expired())
            return;
        step_.reset();
        startLifecycleStep(runtime,
                           deadline,
                           step_,
                           std::move(root),
                           [self = shared_from_this()] { return self->writeWanted(); });
        std::atomic_store_explicit(&active_, step_, std::memory_order_release);
    }

    bool published(uint64_t sequence) const noexcept { return written_.load(std::memory_order_acquire) >= sequence; }

    // Coordinator. Polls without I/O until the record of this sequence or a
    // later one is written, its write fails, or the deadline expires.
    bool waitPublished(uint64_t sequence, RpcRuntime& runtime, RpcDeadline const& deadline, std::shared_ptr<void> root)
    {
        for(;;)
        {
            pump(runtime, deadline, root);
            if(published(sequence))
                return true;
            if(failed_.load(std::memory_order_acquire) >= sequence || deadline.expired())
                return false;
            std::this_thread::sleep_for(
                    std::min(deadline.remaining(),
                             std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(1))));
        }
    }

    // Cleanup callback of the last original native owner only, after its
    // physical native cleanup. Returns once the Stopped record is written.
    void publishTerminal(Record record)
    {
        // Nonterminal steps run on a runtime that settles before this callback
        // starts; observe that here instead of assuming it.
        for(;;)
        {
            auto active = std::atomic_load_explicit(&active_, std::memory_order_acquire);
            if(!active || active->done.load(std::memory_order_acquire))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for(;;)
        {
            auto current = currentAttempt();
            record.attempt = current.first;
            record.entered = current.second;
            if(write(record))
            {
                terminal_.store(true, std::memory_order_release);
                return;
            }
            // Known local failure: try again only after a new stop request.
            while(attempt_.load(std::memory_order_acquire) == current.first)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    bool terminalPublished() const noexcept { return terminal_.load(std::memory_order_acquire); }

private:
    struct Wanted
    {
        uint64_t sequence;
        Record record;
    };
    explicit LifecyclePublisher(Writer writer)
        : writer_(std::move(writer))
    {}
    bool write(Record const& record) noexcept
    {
        try
        {
            return writer_(record);
        }
        catch(...)
        {
            return false; // Local preparation/allocation failure; nothing renamed.
        }
    }
    int writeWanted() noexcept
    {
        auto wanted = std::atomic_load_explicit(&wanted_, std::memory_order_acquire);
        bool written = write(wanted->record);
        // One step runs at a time and reads the newest record, so each store
        // is at least the previous value.
        (written ? written_ : failed_).store(wanted->sequence, std::memory_order_release);
        if(wanted->record.diagnostic)
        {
            try
            {
                wanted->record.diagnostic();
            }
            catch(...)
            {}
        }
        return written ? CL_SUCCESS : CL_ERR_SHUTDOWN_INCOMPLETE;
    }
    std::pair<uint64_t, Clock::time_point> currentAttempt() const noexcept
    {
        for(;;)
        {
            auto before = version_.load(std::memory_order_acquire);
            auto attempt = attempt_.load(std::memory_order_acquire);
            auto entered = entered_.load(std::memory_order_acquire);
            if(!(before & 1) && version_.load(std::memory_order_acquire) == before)
                return {attempt, Clock::time_point(Clock::duration(entered))};
            std::this_thread::yield();
        }
    }
    Writer const writer_;
    // Coordinator only.
    uint64_t requested_ = 0;
    std::shared_ptr<LifecycleStep> step_;
    // Shared with workers.
    std::shared_ptr<Wanted const> wanted_;
    std::shared_ptr<LifecycleStep> active_;
    std::atomic<uint64_t> written_{0}, failed_{0};
    std::atomic<uint64_t> version_{0}, attempt_{0};
    std::atomic<Clock::rep> entered_{0};
    std::atomic<bool> terminal_{false};
};
} // namespace chronolog::rpc
#endif
