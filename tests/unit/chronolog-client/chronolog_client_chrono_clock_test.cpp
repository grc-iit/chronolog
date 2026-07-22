#include <atomic>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <ChronoClock.h>
#include <ClockState.h>

using chronolog::ChronoClock;
using chronolog::ClockState;
using chronolog::ProcessClock;

namespace
{

int64_t abs_i64(int64_t v) { return v < 0 ? -v : v; }

// Simulate one Cristian round trip and apply it to `clock`. The Visor timeline is
// related to this node's local clock by `true_offset` (visor_tick = node_local +
// true_offset). The node sends when its local clock reads `node_local_t0`; the
// request takes `df` ns to reach the Visor and the reply `db` ns to return. Using
// EVEN df/db keeps RTT/2 exact so the ±RTT/2 uncertainty bound holds without
// integer-rounding slack.
void simulate_sync(ChronoClock& clock, uint64_t node_local_t0, int64_t true_offset, uint64_t df, uint64_t db)
{
    uint64_t t0 = node_local_t0;
    uint64_t visor_T = static_cast<uint64_t>(static_cast<int64_t>(node_local_t0 + df) + true_offset);
    uint64_t t1 = node_local_t0 + df + db;
    clock.applySync(t0, visor_T, t1);
}

} // namespace

// Cristian's algorithm: given a round trip captured as (t0, visor_T, t1),
//   offset      = visor_T + RTT/2 - t1
//   uncertainty = RTT/2          where RTT = t1 - t0.
TEST(ChronoClockTest, AppliesCristianOffsetFromRoundTrip)
{
    ChronoClock clock;
    clock.applySync(/*t0=*/1000, /*visor_T=*/5000, /*t1=*/1100);

    // RTT = 100, RTT/2 = 50, offset = 5000 + 50 - 1100 = 3950.
    EXPECT_EQ(clock.offset(), 3950);

    ClockState s = clock.state();
    EXPECT_EQ(s.visor_time, 5000u);
    EXPECT_EQ(s.offset, 3950);
    EXPECT_EQ(s.uncertainty, 50u);
}

TEST(ChronoClockTest, NegativeOffsetWhenVisorBehindLocal)
{
    ChronoClock clock;
    // Visor tick is far behind the local reading at t1 -> negative offset.
    clock.applySync(/*t0=*/10000, /*visor_T=*/2000, /*t1=*/10000);

    // RTT = 0, offset = 2000 - 10000 = -8000.
    EXPECT_EQ(clock.offset(), -8000);
    EXPECT_EQ(clock.state().uncertainty, 0u);
}

TEST(ChronoClockTest, NowVisorReflectsOffsetWithinLocalBracket)
{
    ChronoClock clock;
    clock.applySync(/*t0=*/0, /*visor_T=*/1000000, /*t1=*/0);
    int64_t off = clock.offset(); // = 1000000

    uint64_t before = ChronoClock::local_now();
    uint64_t nv = clock.now_visor();
    uint64_t after = ChronoClock::local_now();

    // now_visor() == some local reading in [before, after] plus the offset.
    EXPECT_GE(nv, before + static_cast<uint64_t>(off));
    EXPECT_LE(nv, after + static_cast<uint64_t>(off));
}

TEST(ChronoClockTest, DefaultClockHasZeroOffset)
{
    ChronoClock clock;
    EXPECT_EQ(clock.offset(), 0);

    uint64_t before = ChronoClock::local_now();
    uint64_t nv = clock.now_visor();
    uint64_t after = ChronoClock::local_now();

    // With no sync applied, now_visor() is just the local steady reading.
    EXPECT_GE(nv, before);
    EXPECT_LE(nv, after);
}

TEST(ChronoClockTest, DefaultClockIsNotSynced)
{
    ChronoClock clock;
    // Cross-epoch consumers must be able to tell "never synced" from "offset 0".
    EXPECT_FALSE(clock.synced());
    EXPECT_EQ(clock.offset(), 0);
}

TEST(ChronoClockTest, ApplySyncSetsSyncedFlag)
{
    ChronoClock clock;
    // Even a sync that happens to yield offset 0 must flip synced().
    clock.applySync(/*t0=*/1000, /*visor_T=*/1000, /*t1=*/1000);
    EXPECT_TRUE(clock.synced());
    EXPECT_EQ(clock.offset(), 0);
}

TEST(ChronoClockTest, LastSyncWins)
{
    ChronoClock clock;
    clock.applySync(0, 1000, 0); // offset 1000
    clock.applySync(0, 7000, 0); // offset 7000 supersedes
    EXPECT_EQ(clock.offset(), 7000);
    EXPECT_EQ(clock.state().visor_time, 7000u);
}

TEST(ChronoClockTest, ProcessClockIsASingleton)
{
    ProcessClock().applySync(0, 424242, 0);
    EXPECT_EQ(ProcessClock().offset(), 424242);
    // Same instance returned on every call.
    EXPECT_EQ(&ProcessClock(), &ProcessClock());
}

// ---- Injectable-source (fake clock) tests -------------------------------------

TEST(ChronoClockTest, NowVisorFollowsInjectedSource)
{
    uint64_t fake = 0;
    ChronoClock clock([&] { return fake; });
    clock.applySync(/*t0=*/0, /*visor_T=*/1000, /*t1=*/0); // symmetric -> offset +1000

    fake = 5000;
    EXPECT_EQ(clock.now_visor(), 6000u); // 5000 + 1000
    fake = 8000;
    EXPECT_EQ(clock.now_visor(), 9000u); // tracks the injected local clock
}

// The estimated offset must stay within the reported ±RTT/2 uncertainty of the
// true offset, regardless of forward/return delay asymmetry (§7).
TEST(ChronoClockTest, OffsetErrorBoundedByUncertaintyUnderAsymmetricLatency)
{
    struct Case
    {
        int64_t true_offset;
        uint64_t df;
        uint64_t db;
    };
    // Even delays so RTT/2 is exact; mix of symmetric and both asymmetries, and
    // both offset signs.
    const std::vector<Case> cases = {
            {0, 100000, 100000},        // symmetric -> exact
            {2'000'000, 180000, 20000}, // forward-heavy, node behind Visor
            {-3'000'000, 20000, 180000} // return-heavy, node ahead of Visor
    };
    for(auto const& c: cases)
    {
        ChronoClock clock;
        simulate_sync(clock, /*node_local_t0=*/1'000'000, c.true_offset, c.df, c.db);
        int64_t est = clock.offset();
        uint64_t unc = clock.state().uncertainty;
        EXPECT_LE(abs_i64(est - c.true_offset), static_cast<int64_t>(unc))
                << "true_offset=" << c.true_offset << " df=" << c.df << " db=" << c.db << " est=" << est;
    }
}

// Two nodes with different local-clock skews, each synced to a common Visor, agree
// within the sum of their uncertainties at a common instant — the headline
// cross-client-comparability property. Evaluated on injected clocks so it is
// deterministic.
TEST(ChronoClockMultiNodeTest, TwoSkewedNodesAgreeWithinCombinedUncertainty)
{
    uint64_t real_now = 1'000'000'000ULL; // shared "true time" driver
    const uint64_t SKEW_A = 3'000'000ULL; // node A runs 3 ms ahead of true time
    const uint64_t SKEW_B = 0ULL;         // node B on true time

    ChronoClock a([&] { return real_now + SKEW_A; });
    ChronoClock b([&] { return real_now + SKEW_B; });

    // Visor timeline == true time, so visor_tick = node_local - skew => true_offset = -skew.
    simulate_sync(a, real_now + SKEW_A, -static_cast<int64_t>(SKEW_A), /*df=*/150000, /*db=*/50000);
    simulate_sync(b, real_now + SKEW_B, -static_cast<int64_t>(SKEW_B), /*df=*/60000, /*db=*/100000);

    real_now += 10'000'000ULL; // advance 10 ms and read both at the same instant
    int64_t nva = static_cast<int64_t>(a.now_visor());
    int64_t nvb = static_cast<int64_t>(b.now_visor());

    int64_t combined = static_cast<int64_t>(a.state().uncertainty + b.state().uncertainty);
    EXPECT_LE(abs_i64(nva - nvb), combined);

    // Each is also within its own uncertainty of true Visor time.
    EXPECT_LE(abs_i64(nva - static_cast<int64_t>(real_now)), static_cast<int64_t>(a.state().uncertainty));
    EXPECT_LE(abs_i64(nvb - static_cast<int64_t>(real_now)), static_cast<int64_t>(b.state().uncertainty));
}

// A node's local clock is stepped mid-run (the "manipulate the clock in the
// middle" scenario). now_visor() drifts off by the step until a re-sync recovers
// it — validating that periodic re-sync corrects a mid-run clock manipulation.
TEST(ChronoClockMultiNodeTest, MidRunClockStepRecoveredByResync)
{
    uint64_t real_now = 1'000'000'000ULL;
    uint64_t skew = 5'000'000ULL; // node starts 5 ms ahead
    ChronoClock node([&] { return real_now + skew; });

    // Symmetric sync -> offset exactly cancels skew.
    simulate_sync(node, real_now + skew, -static_cast<int64_t>(skew), /*df=*/40000, /*db=*/40000);
    EXPECT_EQ(node.now_visor(), real_now);

    real_now += 2'000'000ULL; // same rate -> still accurate
    EXPECT_EQ(node.now_visor(), real_now);

    // Mid-run manipulation: the node's local clock is stepped forward 8 ms.
    skew += 8'000'000ULL;
    int64_t err_before = static_cast<int64_t>(node.now_visor()) - static_cast<int64_t>(real_now);
    EXPECT_EQ(err_before, 8'000'000); // stale offset -> now_visor 8 ms ahead

    // Re-sync recovers exactly (symmetric round trip).
    simulate_sync(node, real_now + skew, -static_cast<int64_t>(skew), 40000, 40000);
    int64_t err_after = static_cast<int64_t>(node.now_visor()) - static_cast<int64_t>(real_now);
    EXPECT_EQ(err_after, 0);
    EXPECT_LT(abs_i64(err_after), abs_i64(err_before));
}

// ---- monotonic_clamp (the per-client no-backstep guard) ------------------------

TEST(MonotonicClampTest, StrictlyIncreasesEvenWhenRawStepsBackward)
{
    std::atomic<uint64_t> last{0};
    // Raw ticks rise, then jump DOWN (as after a downward offset re-sync).
    const std::vector<uint64_t> raw = {100, 101, 500, 200, 201, 202};
    uint64_t prev_out = 0;
    for(uint64_t r: raw)
    {
        uint64_t out = chronolog::monotonic_clamp(last, r);
        EXPECT_GT(out, prev_out); // never decreases across the backward step
        prev_out = out;
    }
}

TEST(MonotonicClampTest, RepeatedSameRawStillStrictlyIncreases)
{
    std::atomic<uint64_t> last{0};
    EXPECT_EQ(chronolog::monotonic_clamp(last, 1000), 1000u);
    EXPECT_EQ(chronolog::monotonic_clamp(last, 1000), 1001u); // same raw -> +1
    EXPECT_EQ(chronolog::monotonic_clamp(last, 1000), 1002u);
}

TEST(ClockStateTest, DefaultsAreZero)
{
    ClockState s;
    EXPECT_EQ(s.visor_time, 0u);
    EXPECT_EQ(s.offset, 0);
    EXPECT_EQ(s.drift_rate, 0.0);
    EXPECT_EQ(s.uncertainty, 0u);
}
