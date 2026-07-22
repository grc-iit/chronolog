# Visor-Based Clock & Event Ordering

> **Status:** implemented on branch `visor-clock-exchange` (cut from
> `origin/develop` @ `0eb73213`). Built, unit-tested, and validated end-to-end on
> a local single-node deploy. **Not committed/pushed.**
>
> Scope is deliberately the clock model and its ordering consequences. It does
> **not** implement ATW acceptance/rejection, tail-read, watermarks, eviction, or
> the replay planner — those remain separate work items.

---

## 1. Motivation

Before this change ChronoLog had **no clock authority**. Every client stamped
events with a raw local `std::chrono::high_resolution_clock::now()` count
(`StorytellerClient.cpp`), and the Visor told the client nothing about time
(`ConnectResponseMsg` carried only `{error_code, clientId}`). Events are ordered
by the tuple

```
EventSequence = (eventTime, clientId, eventIndex)     // chronolog_types.h
```

used as a `std::map` key throughout (`StoryChunk`, `KeeperTailStore`, the HDF5 row
order, the replay merge). Because `eventTime` was a per-process local count,
**cross-client ordering was only as good as out-of-band NTP** and could silently
drift or step backward.

This work makes the **ChronoVisor the cluster time authority** and re-anchors event
time onto a Visor-provided timeline.

---

## 2. Design

### 2.1 Core thesis

Event time changes from *raw local ns* to a **Visor-anchored tick**:

```
visor_tick = local_steady_now + offset      (zero-order; offset per node)
```

where `offset` is each node's measured correction against the Visor clock. Two
consequences:

- **The ordering machinery does not change.** `EventSequence = (tick, clientId,
  index)` and every `map<EventSequence, …>` store keep working exactly as-is; the
  sort simply becomes *globally meaningful* across clients. No changes to
  `StoryChunk`, `StoryChunkWriter` (HDF5 rows follow map iteration order),
  `KeeperTailStore`, or the replay merge key.
- **Any component that compares its own `now()` to event times breaks unless it
  also knows the Visor clock.** There are exactly two such spots — the Keeper/
  Grapher/Player chunk-decay timer and the Player active-window boundary — which is
  why the clock is exchanged with the daemons, not just the client.

**Decision:** time stays a `uint64_t` scalar on the wire. `LogEvent.eventTime`,
`EventSequence`, `StoryChunk`, and the HDF5 layout are **unchanged**; only the
*value* becomes Visor-anchored, plus a new `ClockState` struct for the sync
protocol. (A first-class `ChronoTick` type with carried uncertainty is a possible
later step; not now.)

### 2.2 Visor clock authority — `VisorClock`

`src/chrono-visor/include/VisorClock.h`: a tiny static class.

```cpp
class VisorClock {
public:
    static uint64_t now()   // authoritative Visor tick in ns, monotonic
    { return steady_clock::now().time_since_epoch().count(); }
};
```

Both Visor engines (the client-portal engine and the keeper-registry engine) run
in the **same Visor process**, so a static `steady_clock` read is globally
consistent for the cluster — no shared object or pointer plumbing needed. The epoch
is arbitrary (the Visor's boot); comparability comes entirely from everyone syncing
to *this* clock.

### 2.3 Wire type — `ClockState`

`client/cpp/include/ClockState.h` (thallium-serializable):

```cpp
struct ClockState {
    uint64_t visor_time;   // Visor authority tick when it served the sync
    int64_t  offset;       // ns added to local steady clock to reach Visor time
    double   drift_rate;   // first-order correction; 0 => zero-order (shipped)
    uint64_t uncertainty;  // ± RTT/2 bound on the offset error
};
```

Carried on the Connect response, the SyncClock response, and the heartbeat
response. On the server side only `visor_time` is populated (the node derives
`offset`/`uncertainty` from its own round-trip timing).

### 2.4 Node clock — `ChronoClock`

`client/cpp/include/ChronoClock.h` — the single place the offset math lives, reused
by client, keeper, grapher, and player.

- `local_now()` — real `steady_clock` reading (ns).
- `now_visor()` — `node_now() + offset` (lock-free: one atomic load).
- `applySync(t0, visor_T, t1)` — **Cristian's algorithm**:
  `offset = visor_T + RTT/2 − t1`, `uncertainty = RTT/2`, where `RTT = t1 − t0`.
- `synced()` — true once at least one exchange completed (distinct from
  "offset 0"); the cross-epoch consumers gate on this (see §2.6).
- `state()` — a consistent snapshot (all fields from the same `applySync` under a
  short mutex; the hot path never touches the mutex).
- **Injectable local source** (test seam): default reads real `steady_clock`;
  `ChronoClock(TimeSource)` injects a fake local clock so tests can simulate
  per-node skew/drift/steps deterministically. Only `now_visor()` routes through
  the source; `local_now()` stays the real clock. See §6.
- `ProcessClock()` — process-wide singleton (each ChronoLog process has one node
  clock; the sync updates it and the timestamp/boundary paths read it).

`monotonic_clamp(atomic& last, raw)` — a free helper that clamps a raw tick to be
strictly increasing against a per-producer high-water mark, so a downward offset
correction after a re-sync can never make a producer emit a decreasing tick.

### 2.5 Clock-exchange paths

**Client ↔ Visor (request/response):**
- **Connect** carries a `ClockState` (`visor_time = VisorClock::now()`);
  `CLIENT_PROTOCOL_VERSION` bumped `2 → 3`. The client brackets the connect RPC
  with `t0`/`t1` and `applySync`.
- A new **`SyncClock`** RPC/`Client::SyncClock()` API for explicit re-sync; the
  client also re-syncs opportunistically at `AcquireStory`.
- `ChronologTimer::getTimestamp()` returns
  `monotonic_clamp(last_tick_, ProcessClock().now_visor())` — Visor-anchored,
  per-client monotonic. `log_event` and `RoundRobinKeeperChoice` (`tick % N`) are
  otherwise untouched (bonus: identical ticks still map to one keeper).

**Daemons ↔ Visor (piggybacked on the heartbeat):**
- The existing Keeper/Grapher/Player→Visor stats heartbeats (10 s) changed from
  one-way (`disable_response` / `ignore_return_value`) to **request/response**
  returning a `ClockState`. Each reg-client brackets the call with `t0`/`t1` and
  `applySync`. The stats message structs did **not** change (offset is computed
  daemon-side, so no `send_time` field was needed).
- The heartbeat RPC is **timed** (5 s) so an unresponsive Visor can't wedge the
  daemon loop; the timeout is caught as `tl::timeout` (which derives from
  `std::exception`, **not** `tl::exception`).

### 2.6 Ordering re-basing + the `synced()` gate

The shared `StoryPipeline::extractDecayedStoryChunks` seals a chunk when
`current_time ≥ acceptanceWindow + chunk.getEndTime()`. `getEndTime()` derives from
event ChronoTicks, so `current_time` must be on the Visor timeline. Re-based sites:

- Keeper decay — `KeeperDataStore::extractDecayedStoryChunks`.
- Grapher decay — `GrapherDataStore::extractDecayedStoryChunks`.
- Player decay — `PlayerDataStore::extractDecayedStoryChunks`.
- Player boundary — `PlayerDataStore::get_active_window_boundary`.

All now read `ProcessClock().now_visor()`. **Each gates on `synced()`**: until the
first clock exchange, `now_visor()` is on the node's local steady-clock epoch (its
own boot), NOT the Visor's — so on multi-node it must not be compared against Visor
ticks. Decay skips until synced; the boundary returns `0` (all-hot) until synced.
The player boundary additionally guards uint64 underflow when Visor-timeline uptime
is still below the acceptance window.

The `retire`/`exit_time` TTL sites are `now`-vs-`now` (local, self-consistent) and
were intentionally left alone.

---

## 3. Implementation — files

**New**
- `client/cpp/include/ClockState.h` — wire struct.
- `client/cpp/include/ChronoClock.h` — node clock, Cristian's math, `synced()`
  flag, `local_reading()` (injectable source; backs `now_visor()` and the RPC
  `t0`/`t1` captures), `monotonic_clamp`, `ProcessClock()` + the test-only
  `envSimClockSource()` hook.
- `src/chrono-visor/include/VisorClock.h` — authority.
- `tests/unit/chronolog-client/chronolog_client_chrono_clock_test.cpp` — unit tests.
- `tests/end-to-end/clock-skew/clock_skew_harness.sh` — Tier-2 skew harness.

**Client**
- `chronolog_client.h` — `CLIENT_PROTOCOL_VERSION 3`; `Client::SyncClock()`.
- `ConnectResponseMsg.h` — `ClockState` field + serialize.
- `rpcVisorClient.h` — connect-time sync; `SyncClock` RPC wrapper.
- `ChronologClientImpl.{h,cpp}` — `SyncClock()`; opportunistic re-sync in
  `AcquireStory`.
- `ChronologClient.cpp` — `Client::SyncClock` forward.
- `StorytellerClient.{h,cpp}` — `ChronologTimer` monotonic clamp via `now_visor()`.

**Visor**
- `ClientPortalService.h` — populate `ClockState` on Connect; `SyncClock` handler.
- `VisorClientPortal.{h,cpp}` — drop the dead `clock_offset` out-param.
- `KeeperRegistryService.h` — heartbeat handlers reply with `ClockState`; drop
  `ignore_return_value`.

**Daemons**
- `KeeperRegClient.h`, `GrapherRegClient.h`, `PlayerRegClient.h` — heartbeat does
  the round-trip sync (timed + `tl::timeout` catch); drop `disable_response`.
- `KeeperDataStore.cpp`, `GrapherDataStore.cpp`, `PlayerDataStore.cpp` — decay/
  boundary re-based onto `now_visor()` with the `synced()` gate.

### Notable decisions / deviations from the original plan
- **No `send_time` field** added to the stats structs: the daemon computes its own
  offset from local `t0`/`t1`, so echoing its send time would be a dead wire field.
- **Grapher decay re-based too:** the plan said "the Grapher needs no ordering
  change," but its decay timer *is* a now-vs-tick comparison, so it was re-based for
  consistency.
- **`ClocksourceCPPStyle`** in `KeeperDataStore.cpp` is dead code (defined, never
  used); left untouched.
- **Test seam in production:** `ChronoClock` exposes an injectable local source and
  `ProcessClock()` consults `CHRONOLOG_SIM_CLOCK_OFFSET_NS` / `_STEP_FILE` at
  construction. This is a narrowly-gated test affordance — the source is empty when
  the env is unset, so production reads the real `steady_clock` with zero overhead.
- **Daemon offset logging:** each reg-client logs its measured offset/uncertainty
  per heartbeat (DEBUG) — useful for ops and required by the Tier-2 oracle.
- The RPC round-trip `t0`/`t1` captures use `ProcessClock().local_reading()` (not
  the static `local_now()`), so an injected/real skew is measured faithfully.

---

## 4. Wire compatibility

Two breaking changes; all components must be rebuilt/redeployed together:
- Client↔Visor: `CLIENT_PROTOCOL_VERSION 2 → 3`. The Visor rejects a mismatched
  client with `CL_ERR_PROTOCOL_VERSION_MISMATCH` (existing machinery).
- Daemon↔Visor heartbeat: one-way → request/response. There is **no** daemon
  protocol-version gate, but the 5 s heartbeat timeout means a new daemon against an
  old (non-responding) Visor degrades to a per-cycle timeout (warn + retry, stays
  unsynced) rather than deadlocking.

---

## 5. Code review findings & fixes

An xhigh-effort review of the diff surfaced 9 findings; the high/medium ones were
fixed:

| # | Severity | Issue | Fix |
|---|----------|-------|-----|
| 1 | HIGH | Player `get_active_window_boundary` underflowed uint64 when Visor-timeline uptime < acceptance window (steady-clock magnitude, not wall-clock) → replay served only the cold tier, missing recent hot events (fires on every fresh cluster for the first ~5 min). | Guard: `now > window ? now - window : 0`. |
| 2 | HIGH | Daemon decay/boundary read `now_visor()` with `offset = 0` until the first heartbeat sync; on multi-node the local steady epoch ≠ Visor's → premature or never-decay. | Added `synced()` flag; decay skips and the boundary returns 0 until synced. |
| 3 | MED | Heartbeat became a blocking request/response with no timeout → an unresponsive Visor could wedge the daemon loop / block shutdown. | `.timed(5s, …)` + `catch(tl::timeout)`. |
| 5 | MED | New daemon vs old (non-responding) Visor would block forever (no daemon version gate). | Mitigated by the timeout (degrades to warn + retry). |
| 6 | LOW | `state()` overlaid the atomic offset onto the mutex-guarded copy → torn snapshot. | Return `last_state_` as-is. |

Deferred (lower severity): #4 long-lived clients never re-sync (needs a periodic
client timer); #7 extra sync under the client mutex in AcquireStory; #8 sync
boilerplate duplicated across 5 sites; #9 shared `last_tick_` CAS contention on the
hot path.

---

## 6. Testing

### 6.1 Tiered strategy and the CLOCK_MONOTONIC caveat

Cross-node comparability **cannot be tested single-node**: on one host every
process shares the boot epoch, so `offset ≈ 0` and the epoch-mismatch bugs are
invisible. A key trap: ChronoLog reads `steady_clock` (**CLOCK_MONOTONIC**), which
`date`/`settimeofday`/NTP/`libfaketime`-default do **not** move, and same-host
containers share the kernel's monotonic clock. So skewing requires one of: an
injectable seam (Tier-1), `LD_PRELOAD libfaketime` with monotonic faking enabled
(Tier-2), or real VMs / Linux time namespaces (Tier-3).

### 6.2 Tier-1 — injectable seam + deterministic unit tests

`ChronoClock(TimeSource)` injects a fake local clock; production is unchanged
(default source = real `steady_clock`). `tests/unit/chronolog-client/
chronolog_client_chrono_clock_test.cpp` — **15 tests, all passing**:

- Offset math: `AppliesCristianOffsetFromRoundTrip`, `NegativeOffsetWhenVisorBehindLocal`,
  `NowVisorReflectsOffsetWithinLocalBracket`, `LastSyncWins`.
- Sync-state: `DefaultClockHasZeroOffset`, `DefaultClockIsNotSynced`,
  `ApplySyncSetsSyncedFlag`, `ProcessClockIsASingleton`.
- Fake clock: `NowVisorFollowsInjectedSource`.
- **Estimator bound:** `OffsetErrorBoundedByUncertaintyUnderAsymmetricLatency` —
  under symmetric and both-direction-asymmetric round trips (both offset signs),
  the estimate stays within the reported ±RTT/2. (Even delays keep RTT/2 exact; the
  integer impl has ±1 ns rounding slack at odd RTT.)
- **Cross-client comparability:** `TwoSkewedNodesAgreeWithinCombinedUncertainty` —
  two differently-skewed nodes synced to a common Visor agree within the sum of
  their uncertainties at a shared instant.
- **Mid-run manipulation:** `MidRunClockStepRecoveredByResync` — a node's local
  clock is stepped +8 ms mid-run; `now_visor()` drifts off by exactly that until a
  re-sync recovers it to zero error.
- **Monotonic clamp:** `StrictlyIncreasesEvenWhenRawStepsBackward`,
  `RepeatedSameRawStillStrictlyIncreases`, `DefaultsAreZero` (ClockState).

### 6.3 Tier-2 — real binaries under injected clock skew

`tests/end-to-end/clock-skew/clock_skew_harness.sh` runs the **real**
visor/keeper/grapher/player + `chrono-bench` clients over real Thallium RPC, with a
per-process monotonic-clock skew injected via `ProcessClock()`'s test-only hook:

- `CHRONOLOG_SIM_CLOCK_OFFSET_NS` — a fixed ns offset added to this process's
  `steady_clock` (start-skew).
- `CHRONOLOG_SIM_CLOCK_STEP_FILE` — a file holding an extra ns offset, re-read at
  most once/second (mid-run steps).

Both are inert when unset (empty source → real `steady_clock`, zero overhead). The
hook exists because `steady_clock`/CLOCK_MONOTONIC is not settable by
date/NTP/libfaketime-default and same-host containers share it, so this is how a
real process's clock gets skewed to what ChronoLog actually reads. Crucially the
skew flows through `ChronoClock::local_reading()`, which backs **both**
`now_visor()` **and** the RPC round-trip `t0`/`t1` captures, so the sync measures
the skew faithfully (a skewed monotonic clock reads skewed everywhere).

**Oracle:** a node skewed by `S` measures offset ≈ `−S` (mapping its skewed local
clock back onto the Visor timeline), and the measured error must fall within the
node's **own reported uncertainty** (`= RTT/2`) — the design guarantee itself,
checked directly rather than against a fixed tolerance.

Three scenarios, **10/10 assertions pass**:

- **Heterogeneous skewed clients** (+120 ms, −75 ms) on an unskewed cluster → each
  measures offset ≈ −skew to within µs, confirming cross-client comparability with
  real binaries.
- **Uniformly skewed daemons** (+200 ms) → keeper/grapher/player each sync to ≈
  −200 ms within their uncertainty; a skewed client on top still writes and syncs;
  all daemons stay alive with no heartbeat timeouts.
- **Mid-run clock step** (+300 ms written to the step file while running) → the
  keeper re-syncs from ≈ 0 to ≈ −300 ms at the next heartbeat, validating recovery
  from a mid-session clock manipulation on the live binaries.

**Finding surfaced by Tier-2:** the busy Keeper's heartbeat round trip is markedly
slower/asymmetric than the Grapher/Player's — uncertainty ≈ 50 ms (RTT ≈ 100 ms) vs
sub-ms — so the Keeper's clock is only accurate to tens of ms. Every measurement is
still *within its reported uncertainty* (the guarantee holds), and tens of ms is far
inside the chunk-decay acceptance window (functionally harmless), but it is a real
effect of making the heartbeat a blocking request/response on the Keeper's
contended main thread (its data-collection xstreams delay the `t1` capture). The
harness prints a NOTE when a node's uncertainty exceeds 10 ms. See §8.

---

## 7. Verification (single-node)

- Full Debug build clean across all components.
- 15/15 `ChronoClock` unit tests pass.
- Deploy + multi-proc `chrono-bench` write: correct payload byte counts; clients
  log a synced offset on connect (coarse) refined by `SyncClock` (tight, ±10–20 µs
  on localhost); daemons healthy through decay cycles; no timeouts/errors/crashes.

---

## 8. Deferred / follow-ups

- Periodic client re-sync timer (bounded drift for long-lived writers — review #4).
- First-order drift correction (only `drift_rate` carried; offset-only shipped).
- De-duplicate the round-trip-sync boilerplate (review #8).
- **Keeper heartbeat clock uncertainty ≈ 50 ms** (Tier-2 finding): the blocking
  sync on the Keeper's contended main thread inflates the measured RTT. Harmless
  for decay, but tightening it (sync off a dedicated/less-contended thread, or a
  separate lightweight sync RPC — design §3) would improve the Keeper's cross-node
  clock accuracy.
- Full write→seal→archive→replay soak across the hot/cold boundary (needs the
  acceptance window to elapse; short runs don't seal chunks).
- Tier-3 real-multi-node / time-namespace validation.
