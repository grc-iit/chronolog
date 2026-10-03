#include "chrono-player/adapter/VisorClockAudit.h"

#include <absl/log/log.h>
#include <chrono>

namespace chronolog::player
{
namespace
{
// Kernel slew is at most 500 ppm; the extra millisecond covers the gap between paired reads.
constexpr uint64_t kSlewPpm = 500;
constexpr uint64_t kReadGapNs = 1'000'000;
// At most one line per replica and transition kind in this window; the next line counts the rest.
constexpr int64_t kLogIntervalNs = 10'000'000'000;

int64_t steadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

TimeReading readPhysical(const Clock& clock)
{
    auto reading = clock.now();
    return reading.ok() ? *reading : TimeReading{};
}

// Keeps every status and bound combination as received so a malformed reading stays inconclusive.
TimeReading fromWire(const v1::TimeReading& wire)
{
    TimeReading reading;
    reading.physical_ns = wire.physical_ns();
    if(wire.has_uncertainty_ns())
        reading.uncertainty_ns = wire.uncertainty_ns();
    reading.status = wire.status() == v1::CLOCK_STATUS_SYNCED     ? ClockStatus::Synced
                     : wire.status() == v1::CLOCK_STATUS_UNSYNCED ? ClockStatus::Unsynced
                                                                  : ClockStatus::Unavailable;
    return reading;
}

const char* reasonName(ClockAuditReason reason)
{
    switch(reason)
    {
        case ClockAuditReason::None:
            return "none";
        case ClockAuditReason::TransportFailure:
            return "transport_failure";
        case ClockAuditReason::MissingIdentity:
            return "missing_identity";
        case ClockAuditReason::MissingPhysical:
            return "missing_physical";
        case ClockAuditReason::Unavailable:
            return "unavailable";
        case ClockAuditReason::Unsynced:
            return "unsynced";
        case ClockAuditReason::MissingBound:
            return "missing_bound";
        case ClockAuditReason::MalformedReading:
            return "malformed_reading";
        case ClockAuditReason::NegativeElapsed:
            return "negative_elapsed";
        case ClockAuditReason::Discontinuity:
            return "discontinuity";
        case ClockAuditReason::Overflow:
            return "overflow";
        case ClockAuditReason::Stale:
            return "stale";
        case ClockAuditReason::Capacity:
            return "capacity";
    }
    return "unknown";
}
} // namespace

VisorClockAudit::VisorClockAudit(std::string identity,
                                 std::shared_ptr<const Clock> physical,
                                 Monotonic monotonic,
                                 ClockAuditOptions options)
    : identity_(std::move(identity))
    , physical_(std::move(physical))
    , monotonic_(monotonic ? std::move(monotonic) : Monotonic(steadyNs))
    , audit_(options)
{}

VisorClockAudit::Bracket VisorClockAudit::begin() const
{
    Bracket bracket;
    bracket.t0 = readPhysical(*physical_);
    bracket.m0_ns = monotonic_();
    return bracket;
}

ClockAuditDecision VisorClockAudit::record(const Bracket& bracket,
                                           bool replied,
                                           const v1::TimeReading* visor,
                                           const internal::v1::ClockResponder& responder)
{
    ClockAuditSample sample;
    sample.m1_ns = monotonic_();
    sample.t1 = readPhysical(*physical_);
    sample.t0 = bracket.t0;
    sample.m0_ns = bracket.m0_ns;
    sample.replied = replied;
    if(visor)
        sample.visor = fromWire(*visor);
    sample.responder = {responder.replica_id(), responder.instance()};
    const auto elapsed = sample.m1_ns > sample.m0_ns ? static_cast<uint64_t>(sample.m1_ns - sample.m0_ns) : 0;
    sample.discontinuity = physicalStepDetected(sample.t0,
                                                sample.t1,
                                                sample.m0_ns,
                                                sample.m1_ns,
                                                elapsed / 1'000'000 * kSlewPpm + kReadGapNs);
    auto result = audit_.record(sample);
    const auto reason = result.decision.reason;
    const bool unattributed = reason == ClockAuditReason::MissingIdentity || reason == ClockAuditReason::Capacity;
    std::lock_guard lock(mutex_);
    // No entry carries these; report them once as coverage without a replica until an attributed reply arrives.
    if(unattributed && unattributed_ != reason)
        result.transition = ClockAuditTransition{ClockAuditTransition::Kind::CoverageLost,
                                                 sample.responder,
                                                 ClockAuditState::Unknown,
                                                 result.decision};
    if(reason != ClockAuditReason::TransportFailure)
        unattributed_ = unattributed ? reason : ClockAuditReason::None;
    if(result.transition)
        pending_.push_back(*result.transition);
    return result.decision;
}

void VisorClockAudit::retain(const google::protobuf::RepeatedPtrField<std::string>& replicas)
{
    if(!replicas.empty())
        audit_.retainReplicas({replicas.begin(), replicas.end()});
}

void VisorClockAudit::report()
{
    const auto now = monotonic_();
    std::vector<ClockAuditTransition> transitions;
    {
        std::lock_guard lock(mutex_);
        transitions.swap(pending_);
    }
    for(auto& stale: audit_.expire(now)) transitions.push_back(std::move(stale));
    for(const auto& transition: transitions) log(transition, now);
}

std::vector<ClockAuditEntry> VisorClockAudit::entries() const { return audit_.entries(monotonic_()); }

bool VisorClockAudit::admit(const std::string& replica,
                            ClockAuditTransition::Kind kind,
                            int64_t now_ns,
                            uint64_t& suppressed)
{
    std::lock_guard lock(mutex_);
    auto [it, fresh] = limits_.try_emplace({replica, kind});
    auto& limit = it->second;
    if(!fresh && now_ns - limit.logged_ns < kLogIntervalNs)
    {
        ++limit.suppressed;
        return false;
    }
    suppressed = limit.suppressed;
    limit = {now_ns, 0};
    return true;
}

void VisorClockAudit::log(const ClockAuditTransition& transition, int64_t now_ns)
{
    // The first eligible sample is not a recovery.
    if(transition.kind == ClockAuditTransition::Kind::ToOk && transition.from == ClockAuditState::Unknown)
        return;
    uint64_t suppressed = 0;
    if(!admit(transition.key.replica_id, transition.kind, now_ns, suppressed))
        return;
    const auto& key = transition.key;
    const auto& decision = transition.decision;
    switch(transition.kind)
    {
        case ClockAuditTransition::Kind::ToAlarm:
        {
            const auto& o = *decision.observation;
            LOG(WARNING) << "clock audit ALARM role=player identity=" << identity_ << " replica=" << key.replica_id
                         << " instance=" << key.instance << " offset_ns=" << o.offset_ns
                         << " local_bound_ns=" << o.local_bound_ns << " visor_bound_ns=" << o.visor_bound_ns
                         << " rtt_ns=" << o.rtt_ns << " threshold_ns=" << o.threshold_ns
                         << " suppressed=" << suppressed;
            break;
        }
        case ClockAuditTransition::Kind::ToOk:
        {
            const auto& o = *decision.observation;
            LOG(INFO) << "clock audit OK role=player identity=" << identity_ << " replica=" << key.replica_id
                      << " instance=" << key.instance << " offset_ns=" << o.offset_ns
                      << " threshold_ns=" << o.threshold_ns << " suppressed=" << suppressed;
            break;
        }
        case ClockAuditTransition::Kind::CoverageLost:
            LOG(WARNING) << "clock audit coverage lost role=player identity=" << identity_
                         << " replica=" << key.replica_id << " instance=" << key.instance
                         << " reason=" << reasonName(decision.reason) << " suppressed=" << suppressed;
            break;
    }
}

} // namespace chronolog::player
