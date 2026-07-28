# Tail-read benchmark design — `live_tail_read` vs. sealed-only

Date: 2026-07-28
Branch: `694-on-demand-tail-read`
Cluster: ares, Slurm job 22420, 6 nodes `ares-comp-[03-08]`, 40 cores / 46 GB each

## 1. What is being compared

`live_tail_read` is a runtime knob in `chrono_keeper.DataStoreInternals`, default
`false` (`conf/default_conf.json.in:93`, `ConfigurationBlocks.h:249`). Both arms
therefore run **the same binary with one config bit flipped** — no cross-build
noise, no code-path selection at compile time.

**OFF (existing tail read).** `KeeperTailStore::getTailSequences` serves only the
sealed-chunk tail. An event is invisible to `playback()` until its chunk seals,
i.e. after `story_chunk_duration_secs + acceptance_window_secs`.

**ON (this branch).** The sealed last-N is unioned with the active timeline
through the new `ActiveTailSource` interface.
`KeeperStoryPipeline::activeTailSequences` walks `storyTimelineMap` newest→oldest
under `sequencingMutex`; `KeeperTailStore::getTailEvents` then resolves every
sequence the sealed tail missed by calling `findActiveEvent`, **each of which
takes `sequencingMutex` again** (`KeeperTailStore.h:170-190`,
`KeeperStoryPipeline.cpp` `findActiveEvent`).

That is the cost model in one line: a depth-`n` tail read landing in the active
window costs `1 + misses` acquisitions of the same mutex the ingestion path
holds. **`playback_n` is the primary cost driver and the sharpest thing to
sweep.**

### Read fan-out

`gather_story_tail` (`client/cpp/src/KeeperTailReader.cpp:13`) is two-phase and
fans out to *every* keeper assigned to the story: phase 1 asks each keeper for
its last-N sequences, phase 2 fetches payloads for the globally-selected
sequences from whichever keeper reported them. With 2 keepers, one `playback()`
is up to 4 RPCs. Results are assembled into a `std::map<EventSequence, Event>`,
so the returned vector is globally sorted in both arms — ordering is not a
differentiator on the client side.

`RecordingGroup::getActiveKeepers` returns all active keepers in the group, so a
shared story is recorded across both keepers and reads genuinely fan out.

## 2. Topology

| Node | Role |
|---|---|
| ares-comp-03 | visor + grapher + player (RecordingGroup 1) |
| ares-comp-04, ares-comp-05 | 2 × keeper |
| ares-comp-06, 07, 08 | client MPI ranks only |

No client shares a node with a keeper, so keeper CPU and RSS are cleanly
attributable — this is what makes the cost half of the study meaningful.

**Host files are hand-written; `--job-id` must NOT be passed.**
`deploy_cluster.sh:465-475` force-overwrites all four host files with
`visor = first node, keepers = ALL nodes, grapher/player = last N` whenever a job
ID is given, which would place clients on keeper nodes.

`deploy_cluster.sh` requires exactly one line in `hosts_grapher` and
`hosts_player` when `NUM_RECORDING_GROUP=1`.

Transport: `ofi+sockets` over the `-40g` network (`detect_hs_net_suffix` appends
the suffix automatically on ares hosts). Identical in both arms. `ofi+verbs` is
available (`mlx5_0` present) but is left as an optional sensitivity run — swapping
transports adds a variable to the A/B.

## 3. Fixed configuration

Identical across every run except the one bit under test:

```
max_story_chunk_size:      4096
story_chunk_duration_secs: 10       seal window = 25 s
acceptance_window_secs:    15
inactive_story_delay_secs: 120
tail_capacity:             65536    (default, retained)
live_tail_read:            false | true    <- THE ONLY VARIABLE
```

Production defaults are 30/60 (90 s seal window); 10/15 is used to keep OFF-arm
runs tractable. A single sensitivity point at 30/60 is optional.

**Sizing constraint from `tail_capacity = 65536`:** total events per shared story
must satisfy `max(playback_n) <= total_events <= 65536` — the lower bound so the
deepest depth sweep is reachable, the upper bound so sealed-tail eviction never
confounds the comparison.

The installed tree at `~/chronolog-install/chronolog/` predates this branch (its
`DataStoreInternals` has neither `tail_capacity` nor `live_tail_read`), so a full
rebuild and reinstall is a prerequisite.

## 4. Experiments

Payload fixed at 4096 B (`-a 4096 -s 4096 -b 4096`) throughout to remove size
variance. Shared story (`-o`) throughout, so all ranks contend on one pipeline —
this is both the realistic tail-read case and the contention case.

### Family A — the win: send→visible latency

| Parameter | Value |
|---|---|
| ranks | 6, 24, 60, 120 (2/8/20/40 per client node) |
| events per rank | 500 (120 ranks × 500 = 60 000 ≤ 65 536) |
| event_interval | 100 ms |
| poll_interval | 50 ms |
| playback_n | set explicitly (never auto) |
| max_wait | 63 s (≥ 2.5 × seal window) |

Metric: send→visible p50 / p90 / p99 / max, plus `never_seen`.

### Family B1 — write-path interference

Fixed 60 ranks, fixed write rate; sweep `poll_interval` ∈ {1000, 200, 50, 10} ms.
This varies *read* pressure independently of *write* pressure using existing
knobs. Metric: record-event throughput, ON vs OFF, at each read pressure.

### Family B2 — read-path service latency (sharpest test)

Fixed 24 ranks, 2000 events each (48 000 total, within the sizing window); sweep
`playback_n` ∈ {64, 256, 1024, 4096, 16384}. Metric: p50 / p99 of the
`playback()` call itself. This is where the `1 + misses` lock cost must appear;
a regression here is the real risk of the feature.

### B3 — keeper cost

1 Hz sampler over ssh on ares-comp-04/05 reading `/proc/<pid>/stat` and
`/proc/<pid>/status` for the duration of every run. Metrics: mean %CPU of one
core, peak RSS.

### Repetitions and ordering

3 reps. Loop order is rep-outermost then arm-outermost, so each rep is
`deploy OFF → run all OFF points → stop → deploy ON → run all ON points → stop`:
6 deploy cycles total. Estimated 3.5–4 h including deploy overhead.

## 5. `perf_bench.cpp` changes

Three additions, no behavior change to `-w` or `-r`.

1. **Time each `playback()` call.** Currently untimed, so read-path service cost
   cannot be separated from poll-interval quantization. Report p50/p90/p99.
2. **Write throughput in latency mode.** `-l` sets `write=false` and
   `read=false`, so the `-p` block divides by a never-started `writeEventTimer`
   and a zero byte count, printing `nan`/`inf`. Accumulate local write-time
   nanoseconds and payload bytes instead.
3. **`--csv <path>`** per-sample rows (rank, event_time, t_seen, latency_ms,
   playback_us) for offline CDFs.

**Constraint discovered while reading `TimerWrapper.h`:** `timeBlock` performs
`MPI_Reduce` *inside every call* (`TimerWrapper.h:60-66`) — it is a collective.
Using it per-event inside the latency loop would make every `log_event` a global
synchronization point and destroy the latency being measured. Changes 1 and 2
therefore accumulate locally with `now_ns()` and perform a **single** reduce in
`report_latency`. (Separately: `e2e_duration`, `duration_min/ave/max` are never
initialized in the `TimerWrapper` constructor and are `+=`'d from indeterminate
values — pre-existing, not addressed here.)

## 6. Validity threats and how each is handled

1. **Poll-interval quantization.** Measured latency = true + U(0, poll_interval).
   At the 500 ms default this swamps an ON-arm result of ~100 ms. Handled by
   using 50 ms in *both* arms (identical bias, negligible against OFF's 25 s)
   plus a poll-interval sensitivity run (10/50/200 ms) on the ON arm to bound the
   residual.

2. **Survivorship bias in the OFF arm — the most dangerous one.** Events that
   never seal before `max_wait` are silently dropped from the sample, biasing
   OFF's mean *downward* and understating the improvement. Handled by
   `max_wait` ≥ 2.5 × seal window, reporting `never_seen` for every run, and
   treating any OFF result with non-zero `never_seen` as a **lower bound**,
   labelled as such in the results.

3. **Poller blocks the writer.** One rank does both on one thread, so a slow
   `playback()` at depth 16384 perturbs the write schedule. Change 1 makes this
   visible rather than silent.

4. **Warmup.** Discard the first seal window of samples in every run.

5. **Sealed-tail eviction.** Bounded by the sizing constraint in §3.

6. **Clock skew:** not applicable. Writer and poller share a process and
   `high_resolution_clock`, and `log_event` returns exactly the value later
   reported by `Event::time()`, so latency is `now_ns() - event.time()` with no
   correction.

## 7. Deliverable

Results tree with raw logs and per-sample CSVs, a summary table
(arm × config → visibility percentiles, playback service latency, write
throughput, keeper CPU/RSS), and latency CDFs.

## 8. Known caveat, out of scope

The headline Family A result is largely predetermined by configuration: OFF ≈ the
25 s seal window by construction, ON ≈ the ingestion tick. The load-bearing
results of this study are B1 and B2. If time runs short, cut Family A reps, not
B2.
