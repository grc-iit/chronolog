---
sidebar_position: 5
title: "Performance Tuning"
---

# Performance Tuning

This page covers the configuration knobs that most directly affect ChronoLog's throughput, latency, and memory usage. All settings below live in `default_conf.json` under the relevant component block.

## Story Chunk Settings

A *story chunk* is the unit of data that flows through the pipeline: Keeper buffers events into chunks, drains them to Grapher, which persists them to storage. Three parameters govern chunk behavior in `DataStoreInternals`.

### `max_story_chunk_size`

Maximum size of a single story chunk in bytes.

| Component | Default |
|---|---|
| ChronoKeeper | `4096` |
| ChronoGrapher | `4096` |
| ChronoPlayer | `4096` |

**Effect**: larger chunks reduce per-chunk overhead (fewer RPC calls, fewer I/O operations) but increase memory usage and the minimum latency before an event reaches persistent storage. Tune this based on your expected event payload size — a good starting point is 4–8× your average event size.

### `story_chunk_duration_secs`

How long (in seconds) a chunk remains open, accumulating events, before it is sealed and drained downstream.

| Component | Default |
|---|---|
| ChronoKeeper | `10` |
| ChronoGrapher | `60` |
| ChronoPlayer | `60` |

**Effect**: shorter durations reduce end-to-end latency from write to persistent storage at the cost of more frequent RPC round-trips. Longer durations improve batching efficiency. The Keeper value is the most latency-sensitive; the Grapher/Player values affect how quickly data becomes queryable.

### `acceptance_window_secs`

Maximum age (in seconds) of an incoming event timestamp, relative to the current wall clock, for the event to be accepted. Events older than this are rejected.

| Component | Default |
|---|---|
| ChronoKeeper | `15` |
| ChronoGrapher | `180` |

**Effect**: a wider window accommodates clock skew between client hosts and the Keeper nodes, and allows late-arriving events to be stored correctly. Narrowing the window tightens the ordering guarantee but rejects events from slow or clock-skewed producers.

### `inactive_story_delay_secs`

How long (in seconds) a story with no new events is kept in memory before being evicted.

| Component | Default |
|---|---|
| ChronoKeeper | `120` |
| ChronoGrapher | `300` |

**Effect**: longer delays keep story state warm, reducing re-initialization cost when a story becomes active again. Shorter delays free memory sooner in workloads with many short-lived or bursty stories.

## Recording Groups

```json
"RecordingGroup": 7
```

Applies to: `chrono_keeper`, `chrono_grapher`, `chrono_player`.

Recording groups are logical partitions that pair Keeper and Grapher instances. In a multi-node deployment, group IDs determine which Grapher drains which Keeper. The default value of `7` is a single-group configuration. When deploying multiple Keeper+Grapher pairs, assign each pair the same group ID and ensure no two pairs share an ID.

## Clock Source

```json
"clock": {
  "clocksource_type": "CPP_STYLE",
  "drift_cal_sleep_sec": 10,
  "drift_cal_sleep_nsec": 0
}
```

### `clocksource_type`

Controls the mechanism used to generate event timestamps across all components.

| Value | Enum | Description |
|---|---|---|
| `"C_STYLE"` | `C_STYLE` (0) | Uses C `gettimeofday`. Portable but lowest resolution. |
| `"CPP_STYLE"` | `CPP_STYLE` (1) | Uses C++ `std::chrono::high_resolution_clock`. **Default.** Good balance of portability and precision. |
| `"TSC"` | `TSC` (2) | Reads the CPU timestamp counter directly. Lowest latency and highest resolution, but requires a stable, invariant TSC across all sockets. Not suitable for systems with dynamic CPU frequency scaling unless `constant_tsc` is set. |

:::caution
`TSC` mode requires that all CPUs in the system have synchronized, invariant TSCs. Verify with `grep -m1 constant_tsc /proc/cpuinfo` on Linux. Do not use `TSC` on virtual machines or heterogeneous CPU clusters.
:::

### `drift_cal_sleep_sec` / `drift_cal_sleep_nsec`

Interval between clock drift calibration runs (default: every 10 seconds). The drift calibration corrects for slow clock drift between the ChronoLog clock and the system wall clock. Reducing this interval increases correction frequency at a small CPU cost; increasing it reduces CPU overhead but allows more drift to accumulate between corrections.

## Shutdown Grace Period

```json
"chrono_visor": {
  "delayed_data_admin_exit_in_secs": 3
}
```

Number of seconds ChronoVisor waits after receiving a shutdown signal before terminating the data administration service. This grace period allows in-flight RPC calls and pending data acknowledgements to complete cleanly. Increase this value if you observe data loss at shutdown under heavy write load.

## Summary of Tuning Recommendations

| Goal | Recommended change |
|---|---|
| Lower write-to-query latency | Reduce `story_chunk_duration_secs` on Keeper (e.g., `5`) and Grapher (e.g., `30`) |
| Higher throughput / better batching | Increase `max_story_chunk_size` (e.g., `16384` or `65536`) |
| Tolerate high clock skew across nodes | Increase `acceptance_window_secs` on Keeper (e.g., `60`) |
| Reduce memory usage with many stories | Decrease `inactive_story_delay_secs` on Keeper and Grapher |
| Minimum timestamp latency on bare-metal HPC | Set `clocksource_type` to `"TSC"` (verify invariant TSC first) |
| Prevent data loss at shutdown under heavy load | Increase `delayed_data_admin_exit_in_secs` to `10` or more |
