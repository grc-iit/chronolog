#ifndef CHRONOLOG_CHRONO_CLOCK_H
#define CHRONOLOG_CHRONO_CLOCK_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

#include "ClockState.h"

namespace chronolog
{

// ChronoClock maps a node's local monotonic clock onto the ChronoVisor timeline.
//
// Every node (client, keeper, grapher, player) reads its own steady_clock via
// local_now(). A round-trip clock exchange with the Visor (Cristian's algorithm)
// yields an offset such that now_visor() == local_now() + offset approximates the
// Visor's authoritative tick. This is the single place the offset math lives; it
// is reused by the client timestamp path and by the keeper/player boundary logic.
//
// now_visor() is lock-free on the hot path (a single atomic load of the offset);
// applySync() and state() take a short mutex only for the observability snapshot.
//
// Testability: the node's local reading is pluggable. The default constructor
// reads the real steady_clock (production); ChronoClock(TimeSource) injects a fake
// local clock so tests can simulate per-node skew/drift/steps deterministically
// without touching the real system clock (which, being CLOCK_MONOTONIC, isn't
// settable anyway). Only the local reading is injected — the offset math is shared.
class ChronoClock
{
public:
    // A node's local monotonic reading in ns (test seam: return a fake local clock).
    using TimeSource = std::function<uint64_t()>;

    // Production: read the real steady_clock.
    ChronoClock() = default;

    // Test/simulation: read `source` as this node's local clock.
    explicit ChronoClock(TimeSource source)
        : source_(std::move(source))
    {}

    // Real local monotonic reading in ns. Same basis on every node (steady_clock),
    // arbitrary per-process epoch — only meaningful once mapped by the offset. Used
    // directly to bracket the RPC round trip (t0/t1); always the true clock.
    static uint64_t local_now()
    {
        return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    // This node's effective local reading in ns: the injected source under test /
    // simulation, the real steady_clock otherwise. Used by now_visor() AND to
    // bracket the RPC round trip (t0/t1), so an injected/real skew is measured
    // faithfully (a skewed monotonic clock reads skewed everywhere). In production
    // (no source) this is exactly local_now().
    uint64_t local_reading() const { return source_ ? source_() : local_now(); }

    // Visor-anchored tick (ns) for this node.
    uint64_t now_visor() const
    {
        int64_t off = offset_.load(std::memory_order_acquire);
        return static_cast<uint64_t>(static_cast<int64_t>(local_reading()) + off);
    }

    // Apply a round-trip clock-exchange sample (Cristian's algorithm):
    //   t0      = local_now() captured just before the sync request
    //   visor_T = Visor authority tick returned in the response
    //   t1      = local_now() captured just after the response
    // Estimated Visor time at local instant t1 is visor_T + RTT/2, so
    //   offset = (visor_T + RTT/2) - t1,   uncertainty = RTT/2.
    void applySync(uint64_t t0, uint64_t visor_T, uint64_t t1)
    {
        uint64_t rtt = (t1 >= t0) ? (t1 - t0) : 0;
        int64_t off = static_cast<int64_t>(visor_T + rtt / 2) - static_cast<int64_t>(t1);
        offset_.store(off, std::memory_order_release);
        synced_.store(true, std::memory_order_release);

        std::lock_guard<std::mutex> lk(meta_mutex_);
        last_state_.visor_time = visor_T;
        last_state_.offset = off;
        last_state_.uncertainty = rtt / 2;
    }

    // True once at least one clock exchange with the Visor has completed. Until
    // then now_visor() is just the local steady clock (offset 0), whose epoch is
    // this process's boot, NOT the Visor's — so it must not be compared against
    // Visor-anchored event ticks across nodes. Cross-epoch consumers (keeper /
    // grapher / player decay, the player active-window boundary) gate on this.
    bool synced() const { return synced_.load(std::memory_order_acquire); }

    int64_t offset() const { return offset_.load(std::memory_order_acquire); }

    // Consistent snapshot of the last applied sync (for observability / the
    // SyncClock result). All fields come from the same applySync under the lock,
    // so offset/visor_time/uncertainty are mutually consistent (no torn read).
    ClockState state() const
    {
        std::lock_guard<std::mutex> lk(meta_mutex_);
        return last_state_;
    }

private:
    TimeSource source_; // empty => real steady_clock (production)
    std::atomic<int64_t> offset_{0};
    std::atomic<bool> synced_{false};
    mutable std::mutex meta_mutex_;
    ClockState last_state_{};
};

// Clamp a raw tick to be strictly increasing against a per-producer high-water
// mark, so a downward offset correction after a re-sync can never make a producer
// emit a decreasing tick. Returns the clamped (always > previous) value. Lock-free.
inline uint64_t monotonic_clamp(std::atomic<uint64_t>& last_tick, uint64_t raw)
{
    uint64_t prev = last_tick.load(std::memory_order_relaxed);
    uint64_t next;
    do {
        next = (raw > prev) ? raw : prev + 1;
    } while(!last_tick.compare_exchange_weak(prev, next, std::memory_order_relaxed));
    return next;
}

// Test-only clock-simulation hook (Tier-2 harness; see visor_clock_exchange.md
// §6.3). When CHRONOLOG_SIM_CLOCK_OFFSET_NS is set, ProcessClock() adds that fixed
// ns offset to this process's local steady clock — skewing a REAL process's
// monotonic clock deterministically, without kernel time namespaces or
// libfaketime (steady_clock/CLOCK_MONOTONIC is otherwise not settable).
// CHRONOLOG_SIM_CLOCK_STEP_FILE (optional) names a file holding an additional ns
// offset, re-read at most once/second, for mid-run clock steps. When neither is
// set the returned source is empty, so ChronoClock uses the real steady_clock with
// zero overhead and zero behavior change.
inline ChronoClock::TimeSource envSimClockSource()
{
    const char* off_env = std::getenv("CHRONOLOG_SIM_CLOCK_OFFSET_NS");
    const char* step_env = std::getenv("CHRONOLOG_SIM_CLOCK_STEP_FILE");
    if(off_env == nullptr && step_env == nullptr)
    {
        return {}; // not simulated -> real steady_clock
    }
    int64_t base = (off_env != nullptr) ? std::strtoll(off_env, nullptr, 10) : 0;
    std::string step_path = (step_env != nullptr) ? std::string(step_env) : std::string();
    return [base, step_path]() -> uint64_t
    {
        uint64_t raw = ChronoClock::local_now();
        int64_t extra = 0;
        if(!step_path.empty())
        {
            static thread_local int64_t cached = 0;
            static thread_local uint64_t next_read = 0;
            if(raw >= next_read)
            {
                std::ifstream f(step_path);
                if(f)
                {
                    f >> cached;
                }
                next_read = raw + 1000000000ULL; // re-read at most once/second
            }
            extra = cached;
        }
        return static_cast<uint64_t>(static_cast<int64_t>(raw) + base + extra);
    };
}

// Process-wide clock singleton. Each ChronoLog process (client, keeper, grapher,
// player) has exactly one node clock; the connect / heartbeat sync updates it and
// the timestamp / boundary paths read it. In production the sim source is empty,
// so this is the real steady_clock.
inline ChronoClock& ProcessClock()
{
    static ChronoClock instance{envSimClockSource()};
    return instance;
}

} // namespace chronolog

#endif
