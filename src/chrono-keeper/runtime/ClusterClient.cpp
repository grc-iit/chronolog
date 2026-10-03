#include "runtime/ClusterClient.h"

#include <set>

#include <absl/log/log.h>
#include <absl/strings/str_cat.h>
#include "KeeperConfig.h"
#include "adapter/Convert.h"
#include "wal/WalJournal.h"

namespace chronolog::keeper
{

namespace iv1 = chronolog::internal::v1;

namespace
{

absl::Status toStatus(const grpc::Status& status)
{
    return absl::Status(static_cast<absl::StatusCode>(status.error_code()), status.error_message());
}

absl::Status toStatus(const v1::ItemStatus& status)
{
    return absl::Status(static_cast<absl::StatusCode>(status.code()), status.message());
}

const char* reasonName(ClockAuditReason reason)
{
    switch(reason)
    {
        case ClockAuditReason::None:
            return "none";
        case ClockAuditReason::TransportFailure:
            return "transport failure";
        case ClockAuditReason::MissingIdentity:
            return "missing responder identity";
        case ClockAuditReason::MissingPhysical:
            return "missing physical reading";
        case ClockAuditReason::Unavailable:
            return "clock unavailable";
        case ClockAuditReason::Unsynced:
            return "clock unsynced";
        case ClockAuditReason::MissingBound:
            return "synced reading without a bound";
        case ClockAuditReason::MalformedReading:
            return "bound on an unsynced reading";
        case ClockAuditReason::NegativeElapsed:
            return "negative elapsed time";
        case ClockAuditReason::Discontinuity:
            return "physical clock step";
        case ClockAuditReason::Overflow:
            return "arithmetic overflow";
        case ClockAuditReason::Stale:
            return "stale observation";
        case ClockAuditReason::Capacity:
            return "too many replicas";
    }
    return "unknown";
}

// CLOCK_REALTIME may slew by at most 500 ppm against the monotonic clock; the fixed part covers the gap
// between reading the two clocks.
constexpr uint64_t kSlewPartsPerMillion = 500;
constexpr uint64_t kStepSlackNs = 100'000;

} // namespace

ClusterClient::ClusterClient(std::shared_ptr<grpc::Channel> channel,
                             Options options,
                             RamJournal& journal,
                             ConfigMembership& membership,
                             const AcquisitionWatcher& acquisitions)
    : stub_(iv1::Cluster::NewStub(std::move(channel)))
    , options_(std::move(options))
    , journal_(journal)
    , membership_(membership)
    , acquisitions_(acquisitions)
    , failure_timeout_ms_(options_.keeper_failure_timeout_ms)
    , fence_timeout_ms_(options_.release_fence_timeout_ms)
    , audit_({.freshness_ns = std::chrono::nanoseconds(4 * options_.interval).count(), .max_replicas = 16})
{
    deadline_ms_ = keeper::heartbeatDeadline(std::chrono::milliseconds(options_.interval),
                                             options_.keeper_failure_timeout_ms,
                                             options_.release_fence_timeout_ms)
                           .count();
    extend_deadline_ms_ = deadline_ms_.load();
}

ClusterClient::~ClusterClient()
{
    thread_.request_stop();
    cv_.notify_all();
}

absl::Status ClusterClient::registerNow()
{
    iv1::RegisterRequest request;
    request.set_recovered_instance(options_.recovered_instance);
    request.set_policy_version(journal_.hasPhysicalPolicy() ? PhysicalPolicy{}.version : 0);
    auto* process = request.mutable_process();
    process->set_process_id(options_.process_id);
    process->set_instance(options_.instance);
    process->set_endpoint(options_.endpoint);
    process->set_role(iv1::PROCESS_ROLE_KEEPER);
    iv1::RegisterResponse response;
    if(auto rpc = invoke(
               [&](auto& stub, auto& context)
               {
                   response.Clear();
                   const auto start = mark();
                   auto status = stub.Register(&context, request, &response);
                   audit(start, status, response);
                   return status;
               },
               heartbeatDeadline());
       !rpc.ok())
        return toStatus(rpc);
    if(auto status = toStatus(response.status()); !status.ok())
        return status;
    if(!response.visor_replicas().empty())
    {
        std::lock_guard lock(rpc_mutex_);
        replicas_.clear();
        for(const auto& endpoint: response.visor_replicas())
            replicas_.push_back(iv1::Cluster::NewStub(rpc::peerChannel(endpoint)));
        std::vector<std::string> endpoints(response.visor_replicas().begin(), response.visor_replicas().end());
        std::sort(endpoints.begin(), endpoints.end());
        // Replies name replicas by id, not endpoint: after a membership change only the replica that just
        // answered is known to remain, and the others return as they generate readings.
        if(!visor_endpoints_.empty() && endpoints != visor_endpoints_)
        {
            std::vector<std::string> keep;
            if(!response.clock_responder().replica_id().empty())
                keep.push_back(response.clock_responder().replica_id());
            audit_.retainReplicas(keep);
        }
        visor_endpoints_ = std::move(endpoints);
    }
    if(!response.has_policy() && journal_.requiresCatalogPolicy())
        return absl::FailedPreconditionError("Register omitted the Catalog physical policy");
    if(response.has_policy())
    {
        const auto& policy = response.policy();
        if(options_.causal_floor_skew_limit_ns != policy.skew_limit_ns())
            return absl::FailedPreconditionError(absl::StrCat("causal_floor_skew_limit_ns=",
                                                              options_.causal_floor_skew_limit_ns,
                                                              " differs from Catalog skew_limit_ns=",
                                                              policy.skew_limit_ns()));
        if(policy.skew_limit_ns() < 0 || policy.hlc_lead_ns() < policy.skew_limit_ns())
            return absl::FailedPreconditionError("invalid Catalog S and D");
        const auto reserve_ns = static_cast<int64_t>(options_.reserve_ahead_ms) * 1'000'000;
        const auto catalog_reserve_ns = policy.hlc_lead_ns() - policy.skew_limit_ns();
        if(reserve_ns != catalog_reserve_ns)
            return absl::FailedPreconditionError(absl::StrCat("reserve_ahead_ms=",
                                                              options_.reserve_ahead_ms,
                                                              " (",
                                                              reserve_ns,
                                                              " ns) differs from Catalog reserve_ahead_ns=",
                                                              catalog_reserve_ns,
                                                              " (S=",
                                                              policy.skew_limit_ns(),
                                                              ", D=",
                                                              policy.hlc_lead_ns(),
                                                              ")"));
        const PhysicalPolicy expected;
        if(policy.version() != expected.version || policy.acceptance_window_ns() != expected.acceptance_window_ns ||
           policy.skew_limit_ns() != expected.skew_limit_ns || policy.hlc_lead_ns() != expected.hlc_lead_ns ||
           policy.uncertainty_cap_ns() < 0 ||
           static_cast<uint64_t>(policy.uncertainty_cap_ns()) != expected.uncertainty_cap_ns)
            return absl::FailedPreconditionError("physical policy constants differ");
        if(auto status = adoptTimeouts(policy); !status.ok())
            return status;
    }
    if(auto* wal = dynamic_cast<WalJournal*>(&journal_))
        if(auto status = wal->recordInstance(options_.instance); !status.ok())
            return status;
    if(response.has_policy() && response.policy().ceiling_ahead_ns() > 0)
    {
        const auto ahead = std::chrono::nanoseconds(response.policy().ceiling_ahead_ns());
        const auto deadline =
                std::min(heartbeatDeadline(),
                         rpc::livenessDeadline({std::chrono::duration_cast<std::chrono::milliseconds>(ahead)}));
        if(deadline < rpc::kMinLivenessDeadline)
            return absl::FailedPreconditionError("ceiling_ahead is too short to renew within a deadline");
        extend_deadline_ms_ = deadline.count();
        journal_.enableDynamic(options_.instance,
                               convert::fromProto(response.ceiling_floor()),
                               response.physical_ceiling_floor_ns(),
                               response.policy().acceptance_budget_ns(),
                               response.policy().hlc_budget_ns());
    }
    applyRoutes(response.routes());
    if(journal_.dynamic())
    {
        auto status = extendNow();
        if(!status.ok())
            return status;
    }
    if(response.has_policy())
        journal_.adoptCatalogSkewLimit(response.policy().skew_limit_ns());
    registered_ = true;
    return absl::OkStatus();
}

absl::Status ClusterClient::heartbeatNow()
{
    iv1::HeartbeatRequest request;
    request.set_process_id(options_.process_id);
    request.set_instance(options_.instance);
    request.set_applied_revision(acquisitions_.appliedRevision());
    request.set_applied_route_revision(journal_.appliedRouteRevision());
    std::set<StoryId> stories;
    for(auto story: journal_.storyIds()) stories.insert(story);
    for(StoryId story: stories)
    {
        auto view = journal_.sealedView(story);
        if(!view.ok())
            continue;
        auto* entry = request.add_story_frontiers();
        entry->set_story_id(story);
        *entry->mutable_sealed_frontier() = convert::toProto(view->sealed);
        *entry->mutable_evicted_below() = convert::toProto(journal_.evictionFloor(story));
        for(const auto& owner: journal_.predecessorOwners(story))
            if(journal_.evictionFloor(story) >= owner.own_cut)
            {
                auto* drain = request.add_story_frontiers();
                drain->set_story_id(story);
                drain->set_drained_instance(owner.instance);
                drain->set_drained_epoch(owner.epoch);
                *drain->mutable_sealed_frontier() = convert::toProto(view->sealed);
                *drain->mutable_evicted_below() = convert::toProto(journal_.evictionFloor(story));
            }
        if(auto* wal = dynamic_cast<WalJournal*>(&journal_))
        {
            auto chunks = wal->sealedChunks();
            std::sort(chunks.begin(),
                      chunks.end(),
                      [](const auto& a, const auto& b) { return a.chunk.start < b.chunk.start; });
            std::optional<Hlc> start;
            Hlc through;
            for(const auto& chunk: chunks)
            {
                if(chunk.chunk.story_id != story)
                    continue;
                if(!chunk.settled)
                    break;
                if(!start)
                {
                    start = chunk.chunk.start;
                    through = *start;
                }
                if(chunk.chunk.start > through)
                    break;
                through = std::max(through, chunk.chunk.end);
            }
            if(start)
            {
                auto* proof = entry->mutable_settlement();
                proof->set_instance(options_.instance);
                *proof->mutable_coverage_start() = convert::toProto(*start);
                *proof->mutable_settled_through() = convert::toProto(through);
                *proof->mutable_first_event() = convert::toProto(wal->firstEvent(story).value_or(Hlc{}));
            }
        }
        for(const auto& frontier: view->frontiers) *entry->add_frontiers() = convert::toProto(frontier);
    }
    iv1::HeartbeatResponse response;
    if(auto rpc = invoke(
               [&](auto& stub, auto& context)
               {
                   request.clear_admission_evidence();
                   for(const auto& key: journal_.drainAdmissionEvidence())
                   {
                       auto* evidence = request.add_admission_evidence();
                       evidence->set_story_id(key.story_id);
                       evidence->set_writer_id(key.writer_id);
                       evidence->set_incarnation(key.incarnation);
                   }
                   response.Clear();
                   const auto start = mark();
                   auto status = stub.Heartbeat(&context, request, &response);
                   audit(start, status, response);
                   return status;
               },
               heartbeatDeadline());
       !rpc.ok())
        return toStatus(rpc);
    applyRoutes(response.routes());
    auto status = toStatus(response.status());
    if(status.code() == absl::StatusCode::kNotFound)
        registered_ = false;
    return status;
}

void ClusterClient::applyRoutes(const google::protobuf::RepeatedPtrField<iv1::RouteUpdate>& routes)
{
    uint64_t revision = 0;
    for(const auto& update: routes)
    {
        // A tombstone is applied whatever its revision and never moves the applied revision (W10.17).
        if(update.tombstoned())
        {
            (void)journal_.dropStory(update.story_id(), true);
            continue;
        }
        auto state = convert::routeState(update);
        journal_.applyRoute(update.story_id(),
                            state,
                            std::find(update.observe_floor().begin(),
                                      update.observe_floor().end(),
                                      options_.process_id) != update.observe_floor().end(),
                            update.revision(),
                            [&] { membership_.setRouteState(update.story_id(), state); });
        revision = std::max(revision, update.revision());
    }
    journal_.acknowledgeRoutes(revision);
}
absl::Status ClusterClient::extendNow()
{
    for(int attempt = 0; attempt < 3; ++attempt)
    {
        iv1::ExtendCeilingRequest request;
        request.set_process_id(options_.process_id);
        request.set_instance(options_.instance);
        request.set_applied_route_revision(journal_.appliedRouteRevision());
        *request.mutable_wanted_hlc() = convert::toProto(journal_.wantedCeiling());
        request.set_realtime_ns(journal_.realtime());
        iv1::ExtendCeilingResponse response;
        auto rpc = invoke(
                [&](auto& stub, auto& context)
                {
                    response.Clear();
                    return stub.ExtendCeiling(&context, request, &response);
                },
                std::chrono::milliseconds(extend_deadline_ms_));
        if(!rpc.ok())
            return toStatus(rpc);
        auto status = toStatus(response.status());
        if(status.ok())
        {
            journal_.extendCeiling(convert::fromProto(response.ceiling()), response.physical_ceiling_ns());
            return status;
        }
        if(response.routes().empty())
            return status;
        applyRoutes(response.routes());
        journal_.acknowledgeRoutes(response.fence_revision());
    }
    return absl::UnavailableError("ceiling fence kept changing");
}
absl::Status ClusterClient::adoptTimeouts(const iv1::MembershipPolicy& policy)
{
    // Zero is an omitted field: a Visor without A9 leaves the timers in force.
    const uint32_t failure =
            policy.keeper_failure_timeout_ms() ? policy.keeper_failure_timeout_ms() : failure_timeout_ms_.load();
    const uint32_t fence =
            policy.release_fence_timeout_ms() ? policy.release_fence_timeout_ms() : fence_timeout_ms_.load();
    if(failure == failure_timeout_ms_ && fence == fence_timeout_ms_)
        return absl::OkStatus();
    const auto deadline = keeper::heartbeatDeadline(options_.interval, failure, fence);
    if(deadline < rpc::kMinLivenessDeadline)
        return absl::FailedPreconditionError(absl::StrCat("Visor keeper_failure_timeout_ms ",
                                                          failure,
                                                          " and release_fence_timeout_ms ",
                                                          fence,
                                                          " leave no heartbeat deadline of ",
                                                          rpc::kMinLivenessDeadline.count(),
                                                          " ms at heartbeat_interval_ms ",
                                                          options_.interval.count()));
    LOG(INFO) << "adopting the Visor's keeper_failure_timeout_ms " << failure << " (was " << failure_timeout_ms_.load()
              << ") and release_fence_timeout_ms " << fence << " (was " << fence_timeout_ms_.load()
              << "); heartbeat deadline " << deadline.count() << " ms";
    failure_timeout_ms_ = failure;
    fence_timeout_ms_ = fence;
    deadline_ms_ = deadline.count();
    return absl::OkStatus();
}

int64_t ClusterClient::monotonicNow() const
{
    if(options_.monotonic)
        return options_.monotonic();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

ClusterClient::ClockMark ClusterClient::mark() const
{
    if(!options_.clock)
        return {};
    ClockMark out;
    if(auto reading = options_.clock->now(); reading.ok())
        out.physical = *reading;
    out.monotonic_ns = monotonicNow();
    return out;
}

TimeReading ClusterClient::physicalReading(const v1::TimeReading& reading)
{
    // Kept as sent, so a malformed reading is reported as such instead of being normalized.
    TimeReading out;
    out.physical_ns = reading.physical_ns();
    if(reading.has_uncertainty_ns())
        out.uncertainty_ns = reading.uncertainty_ns();
    out.status = reading.status() == v1::CLOCK_STATUS_SYNCED     ? ClockStatus::Synced
                 : reading.status() == v1::CLOCK_STATUS_UNSYNCED ? ClockStatus::Unsynced
                                                                 : ClockStatus::Unavailable;
    return out;
}

void ClusterClient::audited(const ClockAuditSample& sample)
{
    auto checked = sample;
    const uint64_t elapsed =
            sample.m1_ns > sample.m0_ns ? static_cast<uint64_t>(sample.m1_ns) - static_cast<uint64_t>(sample.m0_ns) : 0;
    checked.discontinuity = physicalStepDetected(sample.t0,
                                                 sample.t1,
                                                 sample.m0_ns,
                                                 sample.m1_ns,
                                                 elapsed / 1'000'000 * kSlewPartsPerMillion + kStepSlackNs);
    auto result = audit_.record(checked);
    if(result.transition)
    {
        logTransition(*result.transition);
    }
    else if(result.decision.state == ClockAuditState::Alarm)
    {
        LOG_EVERY_N_SEC(WARNING, 60) << "clock audit alarm persists: keeper=" << options_.process_id << "/"
                                     << options_.instance << " visor=" << checked.responder.replica_id << "/"
                                     << checked.responder.instance
                                     << " offset_ns=" << result.decision.observation->offset_ns
                                     << " threshold_ns=" << result.decision.observation->threshold_ns;
    }
    else if(result.decision.reason == ClockAuditReason::MissingIdentity ||
            result.decision.reason == ClockAuditReason::Capacity)
    {
        LOG_EVERY_N_SEC(WARNING, 300) << "clock audit inconclusive: keeper=" << options_.process_id << "/"
                                      << options_.instance << " reason=" << reasonName(result.decision.reason);
    }
}

void ClusterClient::logTransition(const ClockAuditTransition& transition) const
{
    const auto& o = transition.decision.observation;
    switch(transition.kind)
    {
        case ClockAuditTransition::Kind::ToAlarm:
            LOG(WARNING) << "clock audit alarm: keeper=" << options_.process_id << "/" << options_.instance
                         << " visor=" << transition.key.replica_id << "/" << transition.key.instance
                         << " offset_ns=" << o->offset_ns << " threshold_ns=" << o->threshold_ns
                         << " local_bound_ns=" << o->local_bound_ns << " visor_bound_ns=" << o->visor_bound_ns
                         << " rtt_ns=" << o->rtt_ns;
            break;
        case ClockAuditTransition::Kind::ToOk:
            LOG(INFO) << "clock audit ok: keeper=" << options_.process_id << "/" << options_.instance
                      << " visor=" << transition.key.replica_id << "/" << transition.key.instance
                      << " offset_ns=" << o->offset_ns << " threshold_ns=" << o->threshold_ns
                      << " rtt_ns=" << o->rtt_ns;
            break;
        case ClockAuditTransition::Kind::CoverageLost:
            LOG(WARNING) << "clock audit coverage lost: keeper=" << options_.process_id << "/" << options_.instance
                         << " visor=" << transition.key.replica_id << "/" << transition.key.instance
                         << " reason=" << reasonName(transition.decision.reason);
            break;
    }
}

std::vector<ClockAuditEntry> ClusterClient::clockAudit() const { return audit_.entries(monotonicNow()); }

void ClusterClient::start()
{
    thread_ = std::jthread([this](std::stop_token stop) { loop(stop); });
}

void ClusterClient::kick()
{
    {
        std::lock_guard lock(mutex_);
        kicked_ = true;
    }
    cv_.notify_all();
}

void ClusterClient::loop(std::stop_token stop)
{
    using namespace std::chrono_literals;
    auto backoff = 100ms;
    while(!stop.stop_requested())
    {
        std::chrono::milliseconds wait = options_.interval;
        if(!registered_)
        {
            auto status = registerNow();
            if(status.ok())
            {
                backoff = 100ms;
            }
            else
            {
                LOG(WARNING) << "register failed: " << status;
                wait = backoff;
                backoff = std::min<std::chrono::milliseconds>(backoff * 2, 5000ms);
            }
        }
        else if(auto status = heartbeatNow(); !status.ok())
        {
            LOG_EVERY_N_SEC(WARNING, 5) << "heartbeat failed: " << status;
            wait = 500ms;
        }
        if(registered_ && journal_.dynamic())
        {
            (void)extendNow();
            wait = std::min(wait, 500ms);
        }
        if(options_.clock)
            for(const auto& transition: audit_.expire(monotonicNow())) logTransition(transition);
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, stop, wait, [this] { return kicked_; });
        kicked_ = false;
    }
}

} // namespace chronolog::keeper
