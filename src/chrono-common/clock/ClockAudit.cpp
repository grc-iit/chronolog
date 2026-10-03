#include "clock/ClockAudit.h"

#include <algorithm>
#include <limits>

namespace chronolog
{
namespace
{
using Wide = __int128_t;

constexpr Wide kMin = std::numeric_limits<int64_t>::min();
constexpr Wide kMax = std::numeric_limits<int64_t>::max();

bool fits(Wide value) { return value >= kMin && value <= kMax; }

// I8.3: a Synced reading has a finite bound; Unsynced and Unavailable readings have none.
ClockAuditReason readingReason(const TimeReading& reading)
{
    switch(reading.status)
    {
        case ClockStatus::Synced:
            return reading.uncertainty_ns ? ClockAuditReason::None : ClockAuditReason::MissingBound;
        case ClockStatus::Unsynced:
            return reading.uncertainty_ns ? ClockAuditReason::MalformedReading : ClockAuditReason::Unsynced;
        case ClockStatus::Unavailable:
            return reading.uncertainty_ns ? ClockAuditReason::MalformedReading : ClockAuditReason::Unavailable;
    }
    return ClockAuditReason::MalformedReading;
}

ClockAuditDecision inconclusive(ClockAuditReason reason) { return {ClockAuditState::Inconclusive, reason, {}}; }

ClockAuditTransition::Kind transitionKind(ClockAuditState to)
{
    return to == ClockAuditState::Alarm ? ClockAuditTransition::Kind::ToAlarm
           : to == ClockAuditState::Ok  ? ClockAuditTransition::Kind::ToOk
                                        : ClockAuditTransition::Kind::CoverageLost;
}
} // namespace

ClockAuditDecision evaluateClockSample(const ClockAuditSample& sample)
{
    if(!sample.replied)
        return inconclusive(ClockAuditReason::TransportFailure);
    if(sample.responder.replica_id.empty() || sample.responder.instance.empty())
        return inconclusive(ClockAuditReason::MissingIdentity);
    if(!sample.visor)
        return inconclusive(ClockAuditReason::MissingPhysical);
    for(const auto* reading: {&sample.t0, &sample.t1, &*sample.visor})
        if(auto reason = readingReason(*reading); reason != ClockAuditReason::None)
            return inconclusive(reason);

    const Wide rtt = Wide{sample.m1_ns} - sample.m0_ns;
    if(rtt < 0 || sample.t1.physical_ns < sample.t0.physical_ns)
        return inconclusive(ClockAuditReason::NegativeElapsed);
    if(sample.discontinuity)
        return inconclusive(ClockAuditReason::Discontinuity);

    const Wide local_bound = std::max(*sample.t0.uncertainty_ns, *sample.t1.uncertainty_ns);
    const Wide visor_bound = *sample.visor->uncertainty_ns;
    // Doubled quantities keep the half-nanosecond midpoint and RTT/2 exact.
    const Wide offset2 = Wide{2} * sample.visor->physical_ns - (Wide{sample.t0.physical_ns} + sample.t1.physical_ns);
    const Wide threshold2 = Wide{2} * local_bound + Wide{2} * visor_bound + rtt;
    const Wide offset = offset2 / 2;
    const Wide threshold = (threshold2 + 1) / 2;
    if(!fits(rtt) || !fits(offset) || !fits(threshold) || !fits(local_bound) || !fits(visor_bound))
        return inconclusive(ClockAuditReason::Overflow);

    const Wide magnitude2 = offset2 < 0 ? -offset2 : offset2;
    ClockAuditObservation observation{static_cast<int64_t>(offset),
                                      static_cast<int64_t>(rtt),
                                      static_cast<int64_t>(threshold),
                                      static_cast<uint64_t>(local_bound),
                                      static_cast<uint64_t>(visor_bound),
                                      sample.m1_ns};
    return {magnitude2 > threshold2 ? ClockAuditState::Alarm : ClockAuditState::Ok,
            ClockAuditReason::None,
            observation};
}

bool physicalStepDetected(const TimeReading& t0,
                          const TimeReading& t1,
                          int64_t m0_ns,
                          int64_t m1_ns,
                          uint64_t allowance_ns)
{
    if(t0.status == ClockStatus::Unavailable || t1.status == ClockStatus::Unavailable)
        return true;
    const Wide drift = (Wide{t1.physical_ns} - t0.physical_ns) - (Wide{m1_ns} - m0_ns);
    return (drift < 0 ? -drift : drift) > Wide{allowance_ns};
}

ClockAudit::ClockAudit(ClockAuditOptions options)
    : options_(options)
{}

ClockAudit::Result ClockAudit::record(const ClockAuditSample& sample)
{
    Result result{evaluateClockSample(sample), {}};
    if(result.decision.reason == ClockAuditReason::TransportFailure ||
       result.decision.reason == ClockAuditReason::MissingIdentity)
        return result;

    std::lock_guard lock(mutex_);
    auto it = entries_.find(sample.responder.replica_id);
    if(it == entries_.end())
    {
        if(entries_.size() >= options_.max_replicas)
        {
            result.decision.state = ClockAuditState::Inconclusive;
            result.decision.reason = ClockAuditReason::Capacity;
            return result;
        }
        it = entries_.try_emplace(sample.responder.replica_id).first;
        it->second.key = sample.responder;
    }
    auto& entry = it->second;
    const auto from = entry.state;
    if(entry.key.instance != sample.responder.instance)
    {
        // A restarted replica's old estimate says nothing about its new process.
        entry.key = sample.responder;
        entry.last.reset();
    }
    entry.state = result.decision.state;
    entry.reason = result.decision.reason;
    if(result.decision.observation)
        entry.last = result.decision.observation;
    if(result.decision.state == ClockAuditState::Alarm)
        ++entry.violations;
    if(entry.state != from)
        result.transition = ClockAuditTransition{transitionKind(entry.state), entry.key, from, result.decision};
    return result;
}

std::vector<ClockAuditTransition> ClockAudit::expire(int64_t now_mono_ns)
{
    std::vector<ClockAuditTransition> transitions;
    std::lock_guard lock(mutex_);
    for(auto& [replica, entry]: entries_)
    {
        const auto current = snapshot(entry, now_mono_ns);
        if(current.state == entry.state)
            continue;
        transitions.push_back({ClockAuditTransition::Kind::CoverageLost,
                               entry.key,
                               entry.state,
                               inconclusive(ClockAuditReason::Stale)});
        entry.state = current.state;
        entry.reason = current.reason;
    }
    return transitions;
}

void ClockAudit::retainReplicas(const std::vector<std::string>& replica_ids)
{
    std::lock_guard lock(mutex_);
    std::erase_if(entries_,
                  [&](const auto& item)
                  { return std::find(replica_ids.begin(), replica_ids.end(), item.first) == replica_ids.end(); });
}

std::optional<ClockAuditEntry> ClockAudit::entry(const std::string& replica_id, int64_t now_mono_ns) const
{
    std::lock_guard lock(mutex_);
    auto it = entries_.find(replica_id);
    if(it == entries_.end())
        return std::nullopt;
    return snapshot(it->second, now_mono_ns);
}

std::vector<ClockAuditEntry> ClockAudit::entries(int64_t now_mono_ns) const
{
    std::vector<ClockAuditEntry> out;
    std::lock_guard lock(mutex_);
    out.reserve(entries_.size());
    for(const auto& [replica, entry]: entries_) out.push_back(snapshot(entry, now_mono_ns));
    return out;
}

ClockAuditEntry ClockAudit::snapshot(const ClockAuditEntry& entry, int64_t now_mono_ns) const
{
    auto out = entry;
    if(!entry.last)
        return out;
    const Wide age = Wide{now_mono_ns} - entry.last->sampled_mono_ns;
    out.age_ns = static_cast<int64_t>(std::clamp(age, kMin, kMax));
    if((entry.state == ClockAuditState::Ok || entry.state == ClockAuditState::Alarm) &&
       age > Wide{options_.freshness_ns})
    {
        out.state = ClockAuditState::Inconclusive;
        out.reason = ClockAuditReason::Stale;
    }
    return out;
}

} // namespace chronolog
