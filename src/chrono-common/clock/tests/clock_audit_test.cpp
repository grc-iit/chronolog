#include <gtest/gtest.h>

#include <initializer_list>
#include <limits>
#include <thread>

#include "clock/ClockAudit.h"

namespace chronolog
{
namespace
{
constexpr int64_t kMs = 1'000'000;
constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
constexpr int64_t kI64Min = std::numeric_limits<int64_t>::min();

TimeReading synced(int64_t physical_ns, uint64_t bound_ns) { return {physical_ns, bound_ns, ClockStatus::Synced}; }

// The B45 gate 1 bracket: t0=100 ms, t1=104 ms, RTT=4 ms, every bound 1 ms.
ClockAuditSample sample(int64_t visor_ns, std::string replica = "v1", std::string instance = "i1")
{
    ClockAuditSample s;
    s.t0 = synced(100 * kMs, kMs);
    s.t1 = synced(104 * kMs, kMs);
    s.m0_ns = 5'000 * kMs;
    s.m1_ns = 5'004 * kMs;
    s.visor = synced(visor_ns, kMs);
    s.responder = {std::move(replica), std::move(instance)};
    return s;
}

void expectInconclusive(const ClockAuditSample& s, ClockAuditReason reason)
{
    auto decision = evaluateClockSample(s);
    EXPECT_EQ(decision.state, ClockAuditState::Inconclusive);
    EXPECT_EQ(decision.reason, reason);
    EXPECT_FALSE(decision.observation);
}
} // namespace

TEST(ClockAudit, GateOneAlarmsAboveTheExactThreshold)
{
    auto alarm = evaluateClockSample(sample(110 * kMs));
    EXPECT_EQ(alarm.state, ClockAuditState::Alarm);
    ASSERT_TRUE(alarm.observation);
    EXPECT_EQ(alarm.observation->offset_ns, 8 * kMs);
    EXPECT_EQ(alarm.observation->threshold_ns, 4 * kMs);
    EXPECT_EQ(alarm.observation->rtt_ns, 4 * kMs);
    EXPECT_EQ(alarm.observation->local_bound_ns, static_cast<uint64_t>(kMs));
    EXPECT_EQ(alarm.observation->visor_bound_ns, static_cast<uint64_t>(kMs));

    auto equal = evaluateClockSample(sample(106 * kMs));
    EXPECT_EQ(equal.state, ClockAuditState::Ok);
    EXPECT_EQ(equal.observation->offset_ns, 4 * kMs);

    EXPECT_EQ(evaluateClockSample(sample(106 * kMs + 1)).state, ClockAuditState::Alarm);
}

TEST(ClockAudit, HalfNanosecondMidpointIsDecidedExactly)
{
    // Midpoint 0.5 ns and threshold 0.5 ns: offsets of exactly 0.5 ns pass, 1.5 ns alarms.
    ClockAuditSample s;
    s.t0 = synced(0, 0);
    s.t1 = synced(1, 0);
    s.m0_ns = 0;
    s.m1_ns = 1;
    s.responder = {"v1", "i1"};
    s.visor = synced(1, 0);
    EXPECT_EQ(evaluateClockSample(s).state, ClockAuditState::Ok);
    s.visor = synced(0, 0);
    EXPECT_EQ(evaluateClockSample(s).state, ClockAuditState::Ok);
    s.visor = synced(2, 0);
    auto alarm = evaluateClockSample(s);
    EXPECT_EQ(alarm.state, ClockAuditState::Alarm);
    EXPECT_EQ(alarm.observation->offset_ns, 1);
    EXPECT_EQ(alarm.observation->threshold_ns, 1);
    s.visor = synced(-1, 0);
    EXPECT_EQ(evaluateClockSample(s).state, ClockAuditState::Alarm);
}

TEST(ClockAudit, OppositeSignsAlarmSymmetrically)
{
    auto behind = evaluateClockSample(sample(94 * kMs));
    EXPECT_EQ(behind.state, ClockAuditState::Alarm);
    EXPECT_EQ(behind.observation->offset_ns, -8 * kMs);
    EXPECT_EQ(evaluateClockSample(sample(98 * kMs)).state, ClockAuditState::Ok);
    EXPECT_EQ(evaluateClockSample(sample(98 * kMs - 1)).state, ClockAuditState::Alarm);
}

TEST(ClockAudit, AsymmetricDelayProcessingAndForwardingStayInsideTheInterval)
{
    // Perfect clocks with zero bounds: the reading is taken anywhere inside the bracket.
    const int64_t start = 1'700'000'000 * 1'000'000'000LL;
    for(int64_t forward: std::initializer_list<int64_t>{0, 1, 3 * kMs, 40 * kMs})
        for(int64_t processing: std::initializer_list<int64_t>{0, 7, 2 * kMs, 25 * kMs})
            for(int64_t back: std::initializer_list<int64_t>{0, 1, 5 * kMs, 90 * kMs})
            {
                ClockAuditSample s;
                s.t0 = synced(start, 0);
                s.t1 = synced(start + forward + processing + back, 0);
                s.m0_ns = 77;
                s.m1_ns = 77 + forward + processing + back;
                s.visor = synced(start + forward + processing / 2, 0);
                s.responder = {"leader", "i1"};
                auto decision = evaluateClockSample(s);
                EXPECT_EQ(decision.state, ClockAuditState::Ok) << forward << " " << processing << " " << back;
            }
}

TEST(ClockAudit, MonotonicEpochNeverEntersTheOffset)
{
    auto base = sample(110 * kMs);
    auto shifted = base;
    shifted.m0_ns = kI64Max - 4 * kMs;
    shifted.m1_ns = kI64Max;
    auto negative = base;
    negative.m0_ns = kI64Min;
    negative.m1_ns = kI64Min + 4 * kMs;
    auto a = evaluateClockSample(base);
    for(const auto& s: {shifted, negative})
    {
        auto b = evaluateClockSample(s);
        EXPECT_EQ(b.state, a.state);
        EXPECT_EQ(b.observation->offset_ns, a.observation->offset_ns);
        EXPECT_EQ(b.observation->threshold_ns, a.observation->threshold_ns);
    }
}

TEST(ClockAudit, IneligibleReadingsAreInconclusiveNeverZeroBound)
{
    for(auto status: {ClockStatus::Unsynced, ClockStatus::Unavailable})
    {
        const auto reason =
                status == ClockStatus::Unsynced ? ClockAuditReason::Unsynced : ClockAuditReason::Unavailable;
        for(int position = 0; position < 3; ++position)
        {
            auto s = sample(106 * kMs);
            TimeReading& r = position == 0 ? s.t0 : position == 1 ? s.t1 : *s.visor;
            r.status = status;
            r.uncertainty_ns.reset();
            expectInconclusive(s, reason);
            r.uncertainty_ns = 0;
            expectInconclusive(s, ClockAuditReason::MalformedReading);
        }
    }
    for(int position = 0; position < 3; ++position)
    {
        auto s = sample(106 * kMs);
        TimeReading& r = position == 0 ? s.t0 : position == 1 ? s.t1 : *s.visor;
        r.uncertainty_ns.reset();
        expectInconclusive(s, ClockAuditReason::MissingBound);
    }

    auto s = sample(106 * kMs);
    s.visor.reset();
    expectInconclusive(s, ClockAuditReason::MissingPhysical);
    s = sample(106 * kMs);
    s.replied = false;
    expectInconclusive(s, ClockAuditReason::TransportFailure);
    expectInconclusive(sample(106 * kMs, ""), ClockAuditReason::MissingIdentity);
    expectInconclusive(sample(106 * kMs, "v1", ""), ClockAuditReason::MissingIdentity);
}

TEST(ClockAudit, StepsAndNegativeElapsedAreInconclusive)
{
    auto s = sample(106 * kMs);
    s.discontinuity = true;
    expectInconclusive(s, ClockAuditReason::Discontinuity);

    s = sample(106 * kMs);
    std::swap(s.m0_ns, s.m1_ns);
    expectInconclusive(s, ClockAuditReason::NegativeElapsed);
    s = sample(106 * kMs);
    std::swap(s.t0, s.t1);
    expectInconclusive(s, ClockAuditReason::NegativeElapsed);

    const auto t0 = synced(100 * kMs, kMs);
    EXPECT_FALSE(physicalStepDetected(t0, synced(104 * kMs, kMs), 0, 4 * kMs, 0));
    EXPECT_FALSE(physicalStepDetected(t0, synced(104 * kMs + 50, kMs), 0, 4 * kMs, 50));
    EXPECT_TRUE(physicalStepDetected(t0, synced(104 * kMs + 51, kMs), 0, 4 * kMs, 50));
    EXPECT_TRUE(physicalStepDetected(t0, synced(90 * kMs, kMs), 0, 4 * kMs, 50));
    EXPECT_TRUE(physicalStepDetected(t0, {104 * kMs, {}, ClockStatus::Unavailable}, 0, 4 * kMs, 50));
    EXPECT_TRUE(physicalStepDetected(synced(kI64Min, 0), synced(kI64Max, 0), kI64Max, kI64Min, 0));
}

TEST(ClockAudit, Int64LimitsAreInconclusiveNotOverflowed)
{
    auto s = sample(kI64Min);
    s.t0 = synced(kI64Max, 0);
    s.t1 = synced(kI64Max, 0);
    expectInconclusive(s, ClockAuditReason::Overflow);

    s = sample(106 * kMs);
    s.m0_ns = kI64Min;
    s.m1_ns = kI64Max;
    expectInconclusive(s, ClockAuditReason::Overflow);

    s = sample(106 * kMs);
    s.visor->uncertainty_ns = std::numeric_limits<uint64_t>::max();
    expectInconclusive(s, ClockAuditReason::Overflow);
    s = sample(106 * kMs);
    s.t1.uncertainty_ns = static_cast<uint64_t>(kI64Max) + 1;
    expectInconclusive(s, ClockAuditReason::Overflow);

    // Extreme but representable values still decide.
    s = sample(kI64Max);
    s.t0 = synced(kI64Max - 2, 0);
    s.t1 = synced(kI64Max, 0);
    s.visor->uncertainty_ns = 0;
    s.m0_ns = 0;
    s.m1_ns = 2;
    auto decision = evaluateClockSample(s);
    EXPECT_EQ(decision.state, ClockAuditState::Ok);
    EXPECT_EQ(decision.observation->offset_ns, 1);
}

TEST(ClockAudit, FailedAttemptThenAnotherReplicaUsesTheSuccessfulBracket)
{
    ClockAudit audit;
    auto failed = sample(110 * kMs, "v1");
    failed.replied = false;
    auto first = audit.record(failed);
    EXPECT_EQ(first.decision.reason, ClockAuditReason::TransportFailure);
    EXPECT_FALSE(first.transition);
    EXPECT_FALSE(audit.entry("v1", 0));

    // The retry has its own bracket: t0=200 ms, t1=202 ms, V=201 ms.
    ClockAuditSample retry;
    retry.t0 = synced(200 * kMs, kMs);
    retry.t1 = synced(202 * kMs, kMs);
    retry.m0_ns = 6'000 * kMs;
    retry.m1_ns = 6'002 * kMs;
    retry.visor = synced(201 * kMs, kMs);
    retry.responder = {"v2", "i7"};
    auto second = audit.record(retry);
    EXPECT_EQ(second.decision.state, ClockAuditState::Ok);
    EXPECT_EQ(second.decision.observation->offset_ns, 0);
    EXPECT_EQ(second.decision.observation->rtt_ns, 2 * kMs);
    ASSERT_TRUE(second.transition);
    EXPECT_EQ(second.transition->kind, ClockAuditTransition::Kind::ToOk);

    auto entry = audit.entry("v2", 6'002 * kMs);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->state, ClockAuditState::Ok);
    EXPECT_EQ(entry->last->offset_ns, 0);
    EXPECT_EQ(entry->age_ns, 0);
    EXPECT_FALSE(audit.entry("v1", 0));
}

TEST(ClockAudit, ForwardedReplyUpdatesTheGeneratingReplica)
{
    ClockAudit audit;
    // Sent to follower v2, forwarded and answered by leader v1: only v1 is observed.
    audit.record(sample(110 * kMs, "v1"));
    auto leader = audit.entry("v1", 5'004 * kMs);
    ASSERT_TRUE(leader);
    EXPECT_EQ(leader->state, ClockAuditState::Alarm);
    EXPECT_FALSE(audit.entry("v2", 5'004 * kMs));
    EXPECT_EQ(audit.entries(5'004 * kMs).size(), 1U);
}

TEST(ClockAudit, TransitionsAndViolationCounter)
{
    ClockAudit audit;
    auto r = audit.record(sample(110 * kMs));
    ASSERT_TRUE(r.transition);
    EXPECT_EQ(r.transition->kind, ClockAuditTransition::Kind::ToAlarm);
    EXPECT_EQ(r.transition->from, ClockAuditState::Unknown);
    EXPECT_FALSE(audit.record(sample(111 * kMs)).transition);
    EXPECT_EQ(audit.entry("v1", 0)->violations, 2U);

    r = audit.record(sample(106 * kMs));
    ASSERT_TRUE(r.transition);
    EXPECT_EQ(r.transition->kind, ClockAuditTransition::Kind::ToOk);
    EXPECT_EQ(r.transition->from, ClockAuditState::Alarm);
    auto entry = audit.entry("v1", 5'004 * kMs);
    EXPECT_EQ(entry->state, ClockAuditState::Ok);
    EXPECT_EQ(entry->violations, 2U);

    // An Unsynced Visor loses coverage without counting as a violation.
    auto unsynced = sample(200 * kMs);
    unsynced.visor = TimeReading{200 * kMs, {}, ClockStatus::Unsynced};
    r = audit.record(unsynced);
    ASSERT_TRUE(r.transition);
    EXPECT_EQ(r.transition->kind, ClockAuditTransition::Kind::CoverageLost);
    EXPECT_EQ(r.transition->decision.reason, ClockAuditReason::Unsynced);
    entry = audit.entry("v1", 5'004 * kMs);
    EXPECT_EQ(entry->state, ClockAuditState::Inconclusive);
    EXPECT_EQ(entry->reason, ClockAuditReason::Unsynced);
    EXPECT_EQ(entry->violations, 2U);
    EXPECT_FALSE(audit.record(unsynced).transition);
}

TEST(ClockAudit, RestartDiscardsTheOldInstance)
{
    ClockAudit audit;
    audit.record(sample(110 * kMs, "v1", "old"));
    auto restarted = sample(106 * kMs, "v1", "new");
    restarted.visor = TimeReading{106 * kMs, {}, ClockStatus::Unsynced};
    auto r = audit.record(restarted);
    ASSERT_TRUE(r.transition);
    EXPECT_EQ(r.transition->kind, ClockAuditTransition::Kind::CoverageLost);
    EXPECT_EQ(r.transition->key.instance, "new");

    auto entry = audit.entry("v1", 5'004 * kMs);
    EXPECT_EQ(entry->key.instance, "new");
    EXPECT_EQ(entry->state, ClockAuditState::Inconclusive);
    EXPECT_FALSE(entry->last);
    EXPECT_FALSE(entry->age_ns);
    EXPECT_EQ(entry->violations, 1U);

    audit.record(sample(106 * kMs, "v1", "new"));
    entry = audit.entry("v1", 5'004 * kMs);
    EXPECT_EQ(entry->state, ClockAuditState::Ok);
    EXPECT_EQ(entry->last->offset_ns, 4 * kMs);
}

TEST(ClockAudit, MonotonicAgeMarksStaleObservationsInconclusive)
{
    ClockAudit audit({.freshness_ns = 10 * kMs, .max_replicas = 4});
    audit.record(sample(110 * kMs, "v1"));
    audit.record(sample(106 * kMs, "v2"));
    const int64_t sampled = 5'004 * kMs;

    auto fresh = audit.entry("v1", sampled + 10 * kMs);
    EXPECT_EQ(fresh->state, ClockAuditState::Alarm);
    EXPECT_EQ(fresh->age_ns, 10 * kMs);
    EXPECT_TRUE(audit.expire(sampled + 10 * kMs).empty());

    auto stale = audit.entry("v1", sampled + 10 * kMs + 1);
    EXPECT_EQ(stale->state, ClockAuditState::Inconclusive);
    EXPECT_EQ(stale->reason, ClockAuditReason::Stale);
    EXPECT_EQ(stale->age_ns, 10 * kMs + 1);

    auto transitions = audit.expire(sampled + 10 * kMs + 1);
    ASSERT_EQ(transitions.size(), 2U);
    for(const auto& t: transitions)
    {
        EXPECT_EQ(t.kind, ClockAuditTransition::Kind::CoverageLost);
        EXPECT_EQ(t.decision.reason, ClockAuditReason::Stale);
    }
    EXPECT_EQ(transitions[0].from, ClockAuditState::Alarm);
    EXPECT_EQ(transitions[1].from, ClockAuditState::Ok);
    EXPECT_TRUE(audit.expire(sampled + 20 * kMs).empty());

    // A fresh eligible sample restores coverage.
    auto later = sample(106 * kMs, "v2");
    later.m0_ns += 100 * kMs;
    later.m1_ns += 100 * kMs;
    auto r = audit.record(later);
    ASSERT_TRUE(r.transition);
    EXPECT_EQ(r.transition->kind, ClockAuditTransition::Kind::ToOk);
    EXPECT_EQ(r.transition->from, ClockAuditState::Inconclusive);
}

TEST(ClockAudit, MembershipEvictionAndCapacityBoundTheMap)
{
    ClockAudit audit({.freshness_ns = 10 * kMs, .max_replicas = 2});
    audit.record(sample(106 * kMs, "v1"));
    audit.record(sample(106 * kMs, "v2"));
    auto over = audit.record(sample(106 * kMs, "v3"));
    EXPECT_EQ(over.decision.state, ClockAuditState::Inconclusive);
    EXPECT_EQ(over.decision.reason, ClockAuditReason::Capacity);
    EXPECT_FALSE(over.transition);
    EXPECT_FALSE(audit.entry("v3", 0));

    audit.retainReplicas({"v2", "v3"});
    EXPECT_FALSE(audit.entry("v1", 0));
    EXPECT_TRUE(audit.entry("v2", 0));
    EXPECT_EQ(audit.record(sample(106 * kMs, "v3")).decision.state, ClockAuditState::Ok);
    EXPECT_EQ(audit.entries(0).size(), 2U);
}

TEST(ClockAudit, ConcurrentRecordAndRead)
{
    ClockAudit audit;
    std::thread writer(
            [&]
            {
                for(int i = 0; i < 500; ++i) audit.record(sample(i % 2 ? 110 * kMs : 106 * kMs, "v1"));
            });
    std::thread other(
            [&]
            {
                for(int i = 0; i < 500; ++i) audit.record(sample(106 * kMs, "v2", i % 2 ? "a" : "b"));
            });
    for(int i = 0; i < 500; ++i)
    {
        audit.entries(5'004 * kMs);
        audit.expire(5'004 * kMs);
    }
    writer.join();
    other.join();
    EXPECT_EQ(audit.entry("v1", 0)->violations, 250U);
    EXPECT_EQ(audit.entry("v2", 0)->state, ClockAuditState::Ok);
}

} // namespace chronolog
