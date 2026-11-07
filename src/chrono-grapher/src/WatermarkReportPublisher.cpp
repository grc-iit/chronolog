#include <thallium/serialization/stl/map.hpp>
#include <WatermarkReportPublisher.h>
#include <private/RpcCallState.h>
#include <private/StopDependencyEvidence.h>
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
        std::atomic<bool> cleaned{false};
        bool final = false;
        uint64_t contributions = 0;
        std::map<StoryId, StoryWatermarkRegistry::Incarnation> snapshots;
    };
    struct Target
    {
        ServiceId keeper;
        StoryWatermarkRegistry::Contributions stories;
        std::shared_ptr<Attempt> active;
        unsigned failures = 0;
        rpc::RpcDeadline::Clock::time_point eligible{};
        uint64_t contributions = 0, finalContributions = 0;
        std::map<StoryId, StoryWatermarkRegistry::Incarnation> finalSnapshots;
    };
    std::weak_ptr<rpc::RpcBulkOwner> owner;
    std::shared_ptr<StoryWatermarkRegistry> registry;
    std::chrono::seconds interval;
    rpc::RpcDeadline::Clock::time_point lastPublish;
    std::timed_mutex mutex;
    std::map<std::string, std::shared_ptr<Target>> targets;
    std::string cursor;
    std::map<std::string, std::string> parked;
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

    static std::string key(ServiceId const& keeper)
    {
        std::string result;
        keeper.get_service_as_string(result);
        return result + "@" + std::to_string(keeper.getProviderId());
    }
    class Work final: public rpc::OwnedRpcWork<int>
    {
        struct NativeCleanup
        {
            std::shared_ptr<Attempt> attempt;
            ~NativeCleanup() { attempt->cleaned.store(true, std::memory_order_release); }
        } cleanup_; // Declared first: published only AFTER all native members die.
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
            : cleanup_{attempt}
            , owner_(std::move(owner))
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
                    if(core_->parked.count(key(target_->keeper)))
                        return finish(CL_SUCCESS, State::NotSubmitted, false);
                    stories = target_->stories;
                    attempt_->contributions = target_->contributions;
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
                if(owner_->beforeDispatch)
                    owner_->beforeDispatch();
                {
                    std::unique_lock<std::timed_mutex> lock(core_->mutex, std::defer_lock);
                    if(!lock.try_lock_until(operation.deadline().expiresAt()) || operation.deadline().expired())
                        return finish(CL_ERR_DEADLINE_EXCEEDED, State::NotSubmitted, false);
                    // This is the same irrevocable dispatch boundary for both
                    // ordinary and final jobs, including jobs prepared earlier.
                    if(core_->closing || core_->parked.count(key(target_->keeper)))
                        return finish(CL_SUCCESS, State::NotSubmitted, false);
                    status = core_->registry->beginReport(publication_, operation);
                    if(status != CL_SUCCESS || publication_.reports.empty())
                        return finish(status, State::NotSubmitted, false);
                    attempt_->snapshots = publication_.versions;
                    dispatched = true;
                }
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
    auto previous = target->stories.find(story);
    if(previous == target->stories.end() || previous->second != incarnation)
        ++target->contributions;
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
    return publishImpl(supplied, force, false, nullptr);
}
int WatermarkReportPublisher::publishFinal(rpc::RpcDeadline const& supplied)
{
    return publishImpl(supplied, true, true, nullptr);
}
int WatermarkReportPublisher::finish(rpc::RpcDeadline const& supplied, rpc::StopDependencyEvidence const& evidence)
{
    int result = publishImpl(supplied, true, true, &evidence);
    if(result != CL_SUCCESS)
        return result;
    std::unique_lock<std::timed_mutex> lock(core_->mutex, std::defer_lock);
    if(!lock.try_lock_until(supplied.expiresAt()) || supplied.expired())
        return CL_ERR_SHUTDOWN_INCOMPLETE;
    return evidence.valid() && core_->targets.empty() ? CL_SUCCESS : CL_ERR_SHUTDOWN_INCOMPLETE;
}
int WatermarkReportPublisher::park(rpc::PublisherParkProof const& proof, rpc::RpcDeadline const& deadline)
{
    if(!proof.valid())
        return CL_ERR_SHUTDOWN_INCOMPLETE;
    std::unique_lock<std::timed_mutex> lock(core_->mutex, std::defer_lock);
    if(!lock.try_lock_until(deadline.expiresAt()) || deadline.expired())
        return CL_ERR_SHUTDOWN_INCOMPLETE;
    auto [gate, inserted] = core_->parked.emplace(proof.target(), proof.identity());
    if(!inserted && gate->second != proof.identity())
        return CL_ERR_SHUTDOWN_INCOMPLETE;
    auto it = core_->targets.find(proof.target());
    if(it == core_->targets.end() || !it->second->active)
        return CL_SUCCESS;
    auto const& attempt = it->second->active;
    return attempt->state.load(std::memory_order_acquire) != Core::State::Unknown &&
                           attempt->cleaned.load(std::memory_order_acquire)
                   ? CL_SUCCESS
                   : CL_ERR_SHUTDOWN_INCOMPLETE;
}
int WatermarkReportPublisher::publishImpl(rpc::RpcDeadline const& supplied,
                                          bool force,
                                          bool final,
                                          rpc::StopDependencyEvidence const* evidence)
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
    for(std::size_t visited = 0, count = core_->targets.size(); visited < count && !core_->targets.empty(); ++visited)
    {
        auto it = core_->targets.upper_bound(core_->cursor);
        if(it == core_->targets.end())
            it = core_->targets.begin();
        auto previousCursor = core_->cursor;
        core_->cursor = it->first;
        auto target = it->second;
        if(evidence && evidence->contains(it->first))
            core_->parked.emplace(it->first, "terminal");
        if(target->active)
        {
            auto state = target->active->state.load(std::memory_order_acquire);
            if(state == Core::State::Unknown)
            {
                result = CL_ERR_OUTCOME_UNCONFIRMED;
                continue;
            }
            if(state == Core::State::Queued || !target->active->cleaned.load(std::memory_order_acquire))
                continue;
            if(state == Core::State::LocalSendComplete && target->active->final)
            {
                target->finalSnapshots = target->active->snapshots;
                target->finalContributions = target->active->contributions;
            }
            target->failures = state == Core::State::LocalSendComplete ? 0 : std::min(target->failures + 1, 6u);
            target->eligible =
                    now + (target->failures
                                   ? std::chrono::milliseconds(std::min(uint64_t{1000}, owner->policy.attempt_ms) *
                                                               (1u << (target->failures - 1)))
                                   : core_->interval);
            target->active.reset();
        }
        if(evidence && evidence->contains(it->first))
        {
            // Only original native-cleaned or never-dispatched work reaches
            // here. Terminal peer proof never reclaims an unknown/active send.
            core_->targets.erase(it);
            continue;
        }
        if(core_->parked.count(it->first))
            continue;
        if(final && !target->finalSnapshots.empty() && target->finalContributions == target->contributions &&
           std::all_of(target->finalSnapshots.begin(),
                       target->finalSnapshots.end(),
                       [](auto const& item) { return item.second->load(std::memory_order_acquire); }))
            continue;
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
        attempt->final = final;
        target->active = attempt;
        runtime->submit<int>(std::move(reservation), std::make_shared<Core::Work>(owner, core_, target, attempt))
                .releaseToRuntime();
    }
    return result;
}
} // namespace chronolog
