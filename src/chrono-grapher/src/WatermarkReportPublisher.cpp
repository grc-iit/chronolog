#include <thallium/serialization/stl/map.hpp>
#include <WatermarkReportPublisher.h>
#include <private/RpcCallState.h>
#include <chrono_monitor.h>
#include <algorithm>
#include <mutex>
namespace chronolog
{
struct WatermarkReportPublisher::Core
{
    enum class State
    {
        Queued,
        NotSubmitted,
        LocalSendComplete,
        Unknown
    };
    struct Attempt
    {
        std::atomic<State> state{State::Queued};
    };
    struct Target
    {
        ServiceId keeper;
        StoryWatermarkRegistry::Contributions stories;
        std::shared_ptr<Attempt> active;
        unsigned failures = 0;
        rpc::RpcDeadline::Clock::time_point eligible{};
    };
    std::weak_ptr<rpc::RpcBulkOwner> owner;
    std::shared_ptr<StoryWatermarkRegistry> registry;
    std::chrono::seconds interval;
    rpc::RpcDeadline::Clock::time_point lastPublish;
    std::timed_mutex mutex;
    std::map<std::string, std::shared_ptr<Target>> targets;
    std::string cursor;
    std::atomic<bool> closing{false};
    thallium::remote_procedure remote;
    Core(std::shared_ptr<rpc::RpcBulkOwner> const& owner,
         std::shared_ptr<StoryWatermarkRegistry> registry,
         uint32_t interval)
        : owner(owner)
        , registry(std::move(registry))
        , interval(interval)
        , lastPublish(rpc::RpcDeadline::Clock::now())
        , remote(owner->engine->define("report_story_watermarks").disable_response())
    {}

    class Work final: public rpc::OwnedRpcWork<int>
    {
        std::shared_ptr<rpc::RpcBulkOwner> owner_;
        std::shared_ptr<Core> core_;
        std::shared_ptr<Target> target_;
        std::shared_ptr<Attempt> attempt_;
        StoryWatermarkRegistry::Publication publication_;
        rpc::RpcCallState native_; // Dies before arguments and actual owner.
        rpc::RpcResult<int> finish(int status, State state, bool dispatched)
        {
            attempt_->state.store(state, std::memory_order_release);
            if(state == State::Unknown)
                return {status, dispatched, {}};
            // This marker asserts only local source/native settlement.
            return {status, dispatched, 0};
        }

    public:
        Work(std::shared_ptr<rpc::RpcBulkOwner> owner,
             std::shared_ptr<Core> core,
             std::shared_ptr<Target> target,
             std::shared_ptr<Attempt> attempt)
            : owner_(std::move(owner))
            , core_(std::move(core))
            , target_(std::move(target))
            , attempt_(std::move(attempt))
        {}
        ~Work() override
        {
            auto queued = State::Queued;
            attempt_->state.compare_exchange_strong(queued, State::NotSubmitted, std::memory_order_release);
        }
        rpc::RpcResult<int> run(rpc::OperationContext& operation) override
        {
            bool dispatched = false;
            try
            {
                if(owner_->beforeSerialize)
                    owner_->beforeSerialize();
                StoryWatermarkRegistry::Contributions stories;
                {
                    std::unique_lock<std::timed_mutex> lock(core_->mutex, std::defer_lock);
                    if(!lock.try_lock_until(operation.deadline().expiresAt()) || operation.deadline().expired() ||
                       core_->closing)
                        return finish(CL_ERR_DEADLINE_EXCEEDED, State::NotSubmitted, false);
                    stories = target_->stories;
                }
                int status = core_->registry->snapshotFor(stories, operation.deadline(), publication_);
                if(status != CL_SUCCESS || publication_.reports.empty())
                    return finish(status, State::NotSubmitted, false);
                status = owner_->provision("report_story_watermarks",
                                           target_->keeper.getProviderId(),
                                           operation.deadline());
                if(status != CL_SUCCESS)
                    return finish(status, State::NotSubmitted, false);
                std::string address;
                target_->keeper.get_service_as_string(address);
                // A fresh owned route per settled attempt also invalidates stale
                // cached endpoints. An unknown callable is never overwritten.
                native_.bind(*owner_->engine, core_->remote, address, target_->keeper.getProviderId());
                status = core_->registry->beginReport(publication_, operation);
                if(status != CL_SUCCESS || publication_.reports.empty())
                    return finish(status, State::NotSubmitted, false);
                dispatched = true;
                auto timeout = operation.deadline().cappedAttempt(std::chrono::milliseconds(owner_->policy.attempt_ms));
                if(!timeout)
                    return finish(CL_ERR_OUTCOME_UNCONFIRMED, State::Unknown, true);
                native_.callable().timed(*timeout, publication_.reports);
                // No decoded reply exists. Native fields are destroyed by the
                // charged worker only after this successful local send fence.
                return finish(CL_SUCCESS, State::LocalSendComplete, true);
            }
            catch(thallium::timeout const&)
            {
                return finish(dispatched ? CL_ERR_OUTCOME_UNCONFIRMED : CL_ERR_DEADLINE_EXCEEDED,
                              dispatched ? State::Unknown : State::NotSubmitted,
                              dispatched);
            }
            catch(...)
            {
                return finish(dispatched ? CL_ERR_OUTCOME_UNCONFIRMED : CL_ERR_UNKNOWN,
                              dispatched ? State::Unknown : State::NotSubmitted,
                              dispatched);
            }
        }
    };
};
WatermarkReportPublisher::WatermarkReportPublisher(std::shared_ptr<rpc::RpcBulkOwner> const& owner,
                                                   std::shared_ptr<StoryWatermarkRegistry> registry,
                                                   uint32_t interval)
{
    if(!owner || !registry || owner->closing || owner->runtime.expired())
        throw std::invalid_argument("Watermark publisher needs an actual MID owner and registry");
    if(owner->watermarkReports)
        throw std::logic_error("Watermark publisher already exists for this MID");
    core_ = std::make_shared<Core>(owner, std::move(registry), interval);
    owner->watermarkReports = core_;
    struct Dependencies
    {
        std::shared_ptr<void> previous;
        std::shared_ptr<Core> core;
    };
    owner->dependencies = std::make_shared<Dependencies>(Dependencies{owner->dependencies, core_});
}
WatermarkReportPublisher::~WatermarkReportPublisher() = default;
void WatermarkReportPublisher::quiesce() noexcept { core_->closing.store(true, std::memory_order_release); }
void WatermarkReportPublisher::recordContributor(StoryId story,
                                                 ServiceId const& reporter,
                                                 StoryWatermarkRegistry::Incarnation incarnation)
{
    if(!reporter.is_valid())
        return;
    if(!incarnation)
        incarnation = core_->registry->contributorToken(story);
    std::string key;
    reporter.get_service_as_string(key);
    key += "@" + std::to_string(reporter.getProviderId());
    std::lock_guard<std::timed_mutex> lock(core_->mutex);
    // A delayed receive must not replace the current incarnation installed by
    // another receive. Validate here, not before acquiring the publication lock.
    if(!incarnation->load(std::memory_order_acquire))
        return;
    auto& target = core_->targets[key];
    if(!target)
    {
        target = std::make_shared<Core::Target>();
        target->keeper = reporter;
    }
    target->stories[story] = std::move(incarnation);
}
int WatermarkReportPublisher::publish(bool force)
{
    auto owner = core_->owner.lock();
    if(!owner)
        return CL_ERR_SHUTDOWN_INCOMPLETE;
    return publish(rpc::RpcDeadline::after(std::chrono::milliseconds(owner->policy.attempt_ms)), force);
}
int WatermarkReportPublisher::publish(rpc::RpcDeadline const& supplied, bool force)
{
    auto started = rpc::RpcDeadline::Clock::now();
    auto owner = core_->owner.lock();
    auto runtime = owner ? owner->runtime.lock() : nullptr;
    if(!owner || !runtime || owner->closing || core_->closing)
        return CL_ERR_SHUTDOWN_INCOMPLETE;
    auto deadline = rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(owner->policy.attempt_ms));
    if(supplied.expiresAt() < deadline.expiresAt())
        deadline = supplied;
    // Never block a collector execution stream behind another publisher.
    std::unique_lock<std::timed_mutex> lock(core_->mutex, std::try_to_lock);
    if(!lock.owns_lock())
        return CL_ERR_RPC_CAPACITY;
    if(deadline.expired())
        return CL_ERR_DEADLINE_EXCEEDED;
    auto now = rpc::RpcDeadline::Clock::now();
    if(!force && now - core_->lastPublish < core_->interval)
        return CL_SUCCESS;
    core_->lastPublish = now;
    int result = CL_SUCCESS;
    for(std::size_t visited = 0, count = core_->targets.size(); visited < count; ++visited)
    {
        auto it = core_->targets.upper_bound(core_->cursor);
        if(it == core_->targets.end())
            it = core_->targets.begin();
        auto previousCursor = core_->cursor;
        core_->cursor = it->first;
        auto target = it->second;
        if(target->active)
        {
            auto state = target->active->state.load(std::memory_order_acquire);
            if(state == Core::State::Unknown)
            {
                result = CL_ERR_OUTCOME_UNCONFIRMED;
                continue;
            }
            if(state == Core::State::Queued)
                continue;
            target->failures = state == Core::State::LocalSendComplete ? 0 : std::min(target->failures + 1, 6u);
            target->eligible =
                    now + (target->failures
                                   ? std::chrono::milliseconds(std::min(uint64_t{1000}, owner->policy.attempt_ms) *
                                                               (1u << (target->failures - 1)))
                                   : core_->interval);
            target->active.reset();
        }
        if(now < target->eligible && (!force || target->failures != 0))
            continue;
        auto reservation = runtime->reserve(deadline);
        if(!reservation)
        {
            // Revisit this refused target first in the next pass. Advancing
            // beyond it can starve target three behind the same successful pair.
            core_->cursor = std::move(previousCursor);
            result = reservation.status();
            break;
        }
        auto attempt = std::make_shared<Core::Attempt>();
        target->active = attempt;
        runtime->submit<int>(std::move(reservation), std::make_shared<Core::Work>(owner, core_, target, attempt))
                .releaseToRuntime();
    }
    return result;
}
} // namespace chronolog
