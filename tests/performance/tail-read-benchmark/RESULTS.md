# `live_tail_read` vs sealed-only tail read — cluster benchmark results

**Date:** 2026-07-28
**Branch:** `694-on-demand-tail-read` (bench build `57b4b9dc`)
**Cluster:** ares, Slurm job 22420, 6 nodes `ares-comp-[03-08]`
**Raw data:** `results/full_20260728_155801/` (248 MB, gitignored) — `summary.md`,
`summary.csv`, per-sample CSVs, and four figures.

Methodology and the reasoning behind each parameter are in `design.md`.
This file records only what was measured and what it means.

---

## 1. Summary

| | Verdict |
|---|---|
| **Freshness** | **~39× better.** 20.5 s → 0.53 s write-to-visible, flat from 6 to 120 ranks. |
| **Read cost** | **+10% to +24%** on `playback()` service time. Modest. |
| **Write cost** | **No effect on p50. p99 up 4–7× at tail depth ≥ 4096.** The main finding. |
| **Keeper CPU** | **+21%** at 120 ranks (253% → 308% of one core). |
| **Keeper memory** | **No change** (53.3 → 52.8 MB). |

One sentence: the feature does what it claims, costs little at shallow tail
depth, and introduces a write-path latency tail at deep tail depth that did not
exist before.

Both arms are the **same binary**; the only variable is
`chrono_keeper.DataStoreInternals.live_tail_read`.

---

## 2. Setup

**Topology** — no client shares a node with a keeper, so keeper CPU is cleanly
attributable.

| Node | Role |
|---|---|
| ares-comp-03 | visor + grapher + player |
| ares-comp-04, 05 | 2 × keeper |
| ares-comp-06, 07, 08 | client MPI ranks only |

**Fixed configuration** (identical in both arms):

```
story_chunk_duration_secs: 10     seal window = 25 s
acceptance_window_secs:    15
max_story_chunk_size:      4096
tail_capacity:             65536
payload:                   256 B
transport:                 ofi+sockets over the -40g network
```

**Scale:** 3 reps × 2 arms × 13 sweep points = 78 runs.
**Analysis:** 10 s warmup trimmed; the depth sweep additionally trims until the
story has filled to the requested depth.

---

## 3. Family A — freshness (the win)

Per-rank stories, 400 events/rank at 10 ev/s, `playback_n` 256, poll 50 ms.

| ranks | arm | send→visible p50 | p99 | keeper CPU (% core) | keeper RSS (MB) | never seen |
|---|---|---|---|---|---|---|
| 6 | off | 20 706 ms | 25 589 ms | 59.2 | 31.6 | 0 |
| 6 | **on** | **530 ms** | **1 026 ms** | 64.2 | 31.6 | 0 |
| 24 | off | 20 733 ms | 25 615 ms | 133.2 | 34.5 | 0 |
| 24 | **on** | **530 ms** | **1 017 ms** | 151.4 | 34.0 | 0 |
| 60 | off | 20 770 ms | 25 670 ms | 186.2 | 40.8 | 0 |
| 60 | **on** | **534 ms** | **1 025 ms** | 216.9 | 40.1 | 0 |
| 120 | off | 20 548 ms | 25 434 ms | 253.2 | 53.3 | 0 |
| 120 | **on** | **527 ms** | **1 025 ms** | 308.3 | 52.8 | 0 |

**Reading these numbers:**

- **`never seen` is 0 in every cell, both arms.** Neither side is truncated, so
  the comparison is sound. This was the single most important thing to get
  right — dropped samples are always the slowest ones, so a truncated OFF arm
  would have silently *overstated* the improvement.
- OFF's ~20.6 s matches theory exactly: events spread uniformly across a chunk
  wait `acceptance_window (15 s) + chunk_duration/2 (5 s)`.
- ON's ~0.53 s is the background ingestion-collection tick, not an RPC
  round trip. `activeTailSequences` does not drain the ingestion deque, so an
  event is visible only after the next collection tick merges it into the open
  chunk.
- **Latency does not degrade with scale in either arm.** ON is flat at ~530 ms
  from 6 to 120 ranks.
- Memory is unchanged, confirming the "steady-state memory unchanged" claim at
  cluster scale.

---

## 4. Family B2 — cost vs tail depth (the important family)

Shared story, 24 ranks × 2000 events, poll 500 ms, sweeping `playback_n`.

### 4.1 Read path — modest cost

| depth | `playback()` p50 OFF | ON | ratio |
|---|---|---|---|
| 64 | 2 337 µs | 2 317 µs | 0.99× |
| 256 | 4 748 µs | 5 231 µs | 1.10× |
| 1024 | 12 334 µs | 14 034 µs | 1.14× |
| 4096 | 33 266 µs | 45 563 µs | 1.37× |
| 16384 | 103 527 µs | 128 185 µs | 1.24× |

Note both arms scale steeply with depth — OFF alone goes 2.3 ms → 103 ms.
**Depth is expensive regardless of the flag**; `live_tail_read` adds a fraction
on top.

### 4.2 Write path — the regression

| depth | `log_event()` p50 OFF → ON | `log_event()` p99 OFF → ON | p99 ratio |
|---|---|---|---|
| 64 | 247 → 246 µs | 320 → 319 µs | 1.0× |
| 256 | 244 → 247 µs | 315 → 318 µs | 1.0× |
| 1024 | 242 → 244 µs | 332 → 339 µs | 1.0× |
| 4096 | 244 → 244 µs | 673 → **4 772 µs** | **7.1×** |
| 16384 | 239 → 231 µs | 2 477 → **10 805 µs** | **4.4×** |

**The median is untouched at every depth.** Throughput is unaffected; this is
purely a tail/jitter effect. It is invisible below depth 1024 and appears
abruptly at 4096.

---

## 5. Family B1 — read pressure sweep

Shared story, 60 ranks, fixed write rate, sweeping poll interval.

> **The `log_event()` columns in this family are confounded and should not be
> cited.** The OFF arm drops ~80% of samples (`playback_n` 2048 is far below the
> ~12 000 events per chunk period), so it keeps polling for the full 63 s
> `max_wait` against a 60 000-entry pending set. The two arms therefore do
> different amounts of *client-side* work. Family B2 is the clean measurement.

Keeper CPU is still valid here — it is a server-side rate:

| poll interval | keeper CPU OFF | ON | increase |
|---|---|---|---|
| 1000 ms | 74.8% | 273.5% | +266% |
| 200 ms | 268.8% | 499.7% | +86% |
| 50 ms | 439.6% | 642.9% | +46% |
| 10 ms | 466.8% | 675.4% | +45% |

Under sustained tail-read pressure the ON arm consistently costs substantially
more keeper CPU.

---

## 6. Reproducibility

Every headline number reproduces tightly across all three reps.

| measurement | rep 1 | rep 2 | rep 3 |
|---|---|---|---|
| A/120 send→visible p50, OFF | 20 835 ms | 20 173 ms | 20 548 ms |
| A/120 send→visible p50, **ON** | 527 ms | 527 ms | 530 ms |
| A/120 keeper CPU, OFF | 253% | 254% | 253% |
| A/120 keeper CPU, **ON** | 307% | 308% | 314% |
| B2/16384 `playback()` p50, OFF | 103 185 µs | 103 527 µs | 103 703 µs |
| B2/16384 `playback()` p50, **ON** | 130 303 µs | 127 409 µs | 128 185 µs |
| B2/16384 `log_event()` p99, OFF | 2 477 µs | 1 702 µs | 3 501 µs |
| B2/16384 `log_event()` p99, **ON** | 11 344 µs | 10 805 µs | 9 816 µs |

`playback()` p50 varies by under 0.5% within an arm across reps.

---

## 7. Validity — what was controlled

**Ruled out: client-side self-blocking.** In `latency_measure_story` one thread
both writes and polls, so a slow `playback()` delays writes that then fire as a
burst. Since ON's polls are longer, this could have manufactured the write-tail
result. Test: split depth-16384 writes by whether they occurred within 25 ms of
their own rank's `playback()` returning.

| rep | OFF post-poll | OFF clear | ON post-poll | ON clear |
|---|---|---|---|---|
| 1 | 1 644 | 2 532 | 13 339 | 11 195 |
| 2 | 3 464 | 1 528 | 12 388 | 10 617 |
| 3 | 4 166 | 3 400 | 9 074 | 9 852 |

(`log_event()` p99, µs, 10 s warmup trimmed)

Two conclusions:

1. **Post-poll vs clear shows no consistent direction within either arm** — the
   self-blocking artifact is not driving the result.
2. **ON is ~4× worse in both categories, every rep.** Writes ≥25 ms clear of
   their own rank's poll are still 4× slower, so the cause is external to that
   thread. With 24 ranks polling one story concurrently, that points to
   keeper-side contention.

**Also ruled out at depth 16384:** both arms have `never_seen = 0` (identical
returned event volume), and keeper CPU is 279% vs 336% of one core — nowhere
near saturating 40 cores. Neither data volume nor CPU exhaustion explains it.

**Other threats handled:** poll-interval quantization (50 ms in both arms,
identical bias); survivorship bias (`max_wait` 2.5× the seal window, `never_seen`
reported per run and flagged in the summary); warmup (10 s trimmed, plus
fill-to-depth for the depth sweep); tail eviction (story sizes kept under
`tail_capacity`). Clock skew is not applicable — writer and poller share a
process and clock.

---

## 8. What is NOT established

- **The mechanism is inferred, not measured.** The `sequencingMutex` explanation
  comes from reading the code: `activeTailSequences` takes the lock once,
  `findActiveEvent` takes it again per unresolved sequence
  (`KeeperTailStore.h:170-190`), and ingestion contends for the same lock. The
  evidence is consistent with this, but **no lock-wait instrumentation was
  added**, so another serialization on the ON path is not excluded. Settling it
  requires a lock-wait histogram, not another benchmark run.
- **Only one payload size (256 B) and one seal window (10/15 s) were tested.**
  Production defaults are 30/60. Larger payloads would increase the volume
  copied under lock and would likely worsen the write tail.
- **Only 2 keepers.** Read fan-out cost scales with keeper count
  (`gather_story_tail` queries every keeper of the story); this was not swept.
- Family B1's write-path numbers are confounded (§5).

---

## 9. Mitigation options considered — analysis only, nothing implemented

The dominant cost inside the critical section is **N payload copies**
(`out = *event` in `findActiveEvent`), not the lock traffic. At depth 16384 with
256 B payloads that is ~16 384 heap allocations and ~4 MB of memcpy under
`sequencingMutex`.

| Option | Effect | Cost |
|---|---|---|
| **Batch `findActiveEvent` into one acquisition** | Removes N−1 mutex round-trips but leaves all N copies under the lock. Consolidates the same held time into one long hold, so a writer arriving mid-batch waits longer. **Unlikely to fix p99; may worsen single-writer worst case.** | Small, local |
| **Bounded batches** (e.g. 256 per acquisition) | Keeps most round-trip savings; caps writer wait at 256 copies rather than N. | Small, local |
| **Pin chunks, copy outside the lock** | Resolve seqs to `(chunk, LogEvent const*)` under lock, pin the chunks, release, then copy. Moves the O(N) memcpy and allocation out of the critical section entirely — targets the actual dominant cost. | Requires chunk refcounting / `shared_ptr` ownership; seal/decay must respect a pin |
| **Cap effective depth when `live_tail_read` is on** | B2 shows the write tail is identical to sealed-only through depth 1024. A clamp around 1024–2048 buys the freshness win at no measurable write cost. | A few lines. Does not make deep live tail reads fast — makes them not exist |

Any of these can be evaluated directly by re-running `run_bench.sh -f B2`
against a patched keeper and comparing the depth curve.

**Prediction on record:** naive batching moves `playback()` p50 down slightly and
leaves `log_event()` p99 flat or slightly worse; only the pinning variant or the
clamp moves p99 materially.

---

## 10. Bottom line

`live_tail_read` is a clear win for its intended purpose and cheap at shallow to
moderate tail depth. The write-path tail regression is real, reproducible, and
confined to deep tail reads (`playback_n ≥ 4096`) on a story with concurrent
writers. Whether that matters depends entirely on whether deep tail reads are a
real workload — if they are not, a depth clamp closes the issue for a few lines;
if they are, the fix is to get payload copying out of the critical section.
