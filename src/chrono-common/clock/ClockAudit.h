#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "chronolog/types.h"

namespace chronolog
{

// Observational audit of the local physical clock against Visor physical readings (I8.3, section 8).
// Nothing here feeds ClockStatus, TimeReading bounds, L, HLC, policy, frontiers or acceptance.

struct ClockResponderKey
{
    std::string replica_id;
    std::string instance;
    auto operator<=>(const ClockResponderKey&) const = default;
};

// One RPC attempt bracketed by local physical readings t0/t1 and monotonic readings m0/m1.
struct ClockAuditSample
{
    TimeReading t0;
    TimeReading t1;
    int64_t m0_ns{};
    int64_t m1_ns{};
    // False when the attempt failed in transport; the remaining reply fields are then ignored.
    bool replied{true};
    // The Visor's physical reading; absent when the reply carried none.
    std::optional<TimeReading> visor;
    // The replica that generated the reading; empty fields mean no identity.
    ClockResponderKey responder;
    // Set by the injected step detector for a physical discontinuity inside the bracket.
    bool discontinuity{};
};

enum class ClockAuditState
{
    Unknown,
    Ok,
    Alarm,
    Inconclusive
};

enum class ClockAuditReason
{
    None,
    TransportFailure,
    MissingIdentity,
    MissingPhysical,
    Unavailable,
    Unsynced,
    MissingBound,
    MalformedReading,
    NegativeElapsed,
    Discontinuity,
    Overflow,
    Stale,
    Capacity
};

// An eligible observation. offset_ns is truncated toward zero and threshold_ns rounded up;
// the alarm decision itself compares doubled values exactly.
struct ClockAuditObservation
{
    int64_t offset_ns{};
    int64_t rtt_ns{};
    int64_t threshold_ns{};
    uint64_t local_bound_ns{};
    uint64_t visor_bound_ns{};
    int64_t sampled_mono_ns{};
};

struct ClockAuditDecision
{
    ClockAuditState state{ClockAuditState::Inconclusive};
    ClockAuditReason reason{ClockAuditReason::None};
    std::optional<ClockAuditObservation> observation;
};

// Pure evaluation of one sample: Ok or Alarm with an observation, or Inconclusive with a reason.
ClockAuditDecision evaluateClockSample(const ClockAuditSample& sample);

// Step detector: physical elapsed differs from monotonic elapsed by more than allowance_ns.
// Readings without a usable physical value count as a discontinuity.
bool physicalStepDetected(const TimeReading& t0,
                          const TimeReading& t1,
                          int64_t m0_ns,
                          int64_t m1_ns,
                          uint64_t allowance_ns);

struct ClockAuditEntry
{
    ClockResponderKey key;
    ClockAuditState state{ClockAuditState::Unknown};
    ClockAuditReason reason{ClockAuditReason::None};
    // Latest eligible observation of this instance.
    std::optional<ClockAuditObservation> last;
    // Monotonic age of the latest eligible observation at the query time.
    std::optional<int64_t> age_ns;
    // Eligible violations of this replica across instances.
    uint64_t violations{};
};

struct ClockAuditTransition
{
    enum class Kind
    {
        ToAlarm,
        ToOk,
        CoverageLost
    };
    Kind kind{};
    ClockResponderKey key;
    ClockAuditState from{};
    ClockAuditDecision decision;
};

struct ClockAuditOptions
{
    // An eligible observation older than this on the monotonic clock is Inconclusive.
    int64_t freshness_ns{60'000'000'000};
    size_t max_replicas{16};
};

// Bounded per-replica audit state. Thread safe.
class ClockAudit
{
public:
    explicit ClockAudit(ClockAuditOptions options = {});

    struct Result
    {
        ClockAuditDecision decision;
        std::optional<ClockAuditTransition> transition;
    };

    // Evaluates one sample and, when it carries identity, updates its replica's entry.
    Result record(const ClockAuditSample& sample);
    // Marks observations older than the freshness limit Inconclusive.
    std::vector<ClockAuditTransition> expire(int64_t now_mono_ns);
    // Drops replicas outside the membership.
    void retainReplicas(const std::vector<std::string>& replica_ids);

    std::optional<ClockAuditEntry> entry(const std::string& replica_id, int64_t now_mono_ns) const;
    std::vector<ClockAuditEntry> entries(int64_t now_mono_ns) const;

private:
    ClockAuditEntry snapshot(const ClockAuditEntry& entry, int64_t now_mono_ns) const;

    ClockAuditOptions options_;
    mutable std::mutex mutex_;
    std::map<std::string, ClockAuditEntry> entries_;
};

} // namespace chronolog
