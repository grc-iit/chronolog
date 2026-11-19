---
sidebar_position: 5
title: "Performance Tuning"
---

# Performance Tuning

This page covers the configuration knobs that most directly affect ChronoLog's throughput, latency, and memory usage. All settings below live in `default_conf.json` under the relevant component block.

## Story Chunk Settings

A *story chunk* is the unit of data that flows through the pipeline: Keeper buffers events into chunks, drains them to Grapher, which persists them to storage. These settings live in `DataStoreInternals`. The defaults are the template's; a key left out of the file takes a different compiled value (see [Server Configuration → DataStoreInternals](./server-configuration.md#datastoreinternals--story-chunk-tuning)). If you change them, keep the [timing rules across components](./server-configuration.md#timing-rules-across-components).

### `max_story_chunk_size`

Not used for now. Every component reads it (the template sets `4096`), but nothing limits a chunk by its number of events: a chunk covers a time range of `story_chunk_duration_secs`, so changing this key has no effect.

### `story_chunk_duration_secs`

How long (in seconds) a chunk remains open, accumulating events, before it is sealed and drained downstream.

| Component | Default |
|---|---|
| ChronoKeeper | `10` |
| ChronoGrapher | `30` |
| ChronoPlayer | `60` |

**Effect**: shorter durations reduce end-to-end latency from write to persistent storage at the cost of more frequent RPC round-trips. Longer durations improve batching efficiency. The ChronoKeeper value is the most latency-sensitive; the ChronoGrapher/ChronoPlayer values affect how quickly data becomes queryable.

### `acceptance_window_secs`

Maximum age (in seconds) of an incoming event timestamp, relative to the current wall clock, for the event to be accepted. Events older than this are rejected.

| Component | Default |
|---|---|
| ChronoKeeper | `15` |
| ChronoGrapher | `60` |
| ChronoPlayer | `300` |

**Effect**: a wider window accommodates clock skew between client hosts and the ChronoKeeper nodes, and allows late-arriving events to be stored correctly. Narrowing the window tightens the ordering guarantee but rejects events from slow or clock-skewed producers.

### `inactive_story_delay_secs`

How long (in seconds) a story with no new events is kept in memory before being evicted.

| Component | Default |
|---|---|
| ChronoKeeper | `120` |
| ChronoGrapher | `300` |
| ChronoPlayer | `300` |

**Effect**: longer delays keep story state warm, reducing re-initialization cost when a story becomes active again. Shorter delays free memory sooner in workloads with many short-lived or bursty stories.

## Ingestion Thread Count

Each component's ingestion RPC service exposes an `IngestionThreadCount` knob that controls how many worker threads serve incoming requests.

| Component                                       | Default | Tunes                                                                          |
| ----------------------------------------------- | ------- | ------------------------------------------------------------------------------ |
| `KeeperRecordingService.IngestionThreadCount`   | `4`     | Concurrent client → Keeper event ingestion. Raise for high client fan-in.      |
| `KeeperGrapherDrainService.IngestionThreadCount`| `1`     | Concurrent Keeper → Grapher chunk drain. Raise for multi-Keeper deployments.   |
| `PlaybackQueryService.IngestionThreadCount`     | `1`     | Concurrent client → Player playback queries. Raise for concurrent readers.     |

**Effect**: more threads improve RPC concurrency at the cost of CPU and contention on internal queues. The Keeper default (`4`) reflects the heavier ingestion workload; Grapher and Player default to `1` and only need raising when the corresponding traffic is concurrent.

## Extraction Pipeline

The `ExtractionModule` block in `chrono_keeper` and `chrono_grapher` controls how retired StoryChunks are drained downstream. Two knobs affect throughput directly:

### `extraction_stream_count`

Number of parallel worker threads draining the extraction queue through the configured extractor chain.

| Component     | Default |
| ------------- | ------- |
| ChronoKeeper  | `2`     |
| ChronoGrapher | `2`     |

A file without the key gets `1`.

**Effect**: higher values increase chunk-throughput when downstream extractors (CSV writes, RDMA fan-out, HDF5 archive) are the bottleneck. The benefit plateaus when storage or network bandwidth saturates.

### `extractors`

The composition of the extractor chain itself is a tuning surface: chains can be shortened (drop CSV mirroring), lengthened (add a parallel RDMA target), or swapped (use `dual_endpoint_rdma_extractor` to deliver chunks to both ChronoGrapher and ChronoPlayer in one pass). See [Server Configuration → ExtractionModule](./server-configuration.md#extractionmodule) for the schema.

## Recording Groups

```json
"RecordingGroup": 7
```

Applies to: `chrono_keeper`, `chrono_grapher`, `chrono_player`.

Recording groups are logical partitions that pair ChronoKeeper and ChronoGrapher instances. In a multi-node deployment, recordin group IDs determine which ChronoGrapher drains which ChronoKeeper. The default value of `7` is a single-group configuration. When deploying multiple ChronoKeeper+ChronoGrapher pairs, assign each pair the same group ID and ensure no two pairs share the same ID.

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

Interval between clock drift calibration runs (default: every 10 seconds). The drift calibration corrects for slow clock drift between the ChronoLog clock and the system wall clock. Reducing this interval increases correction frequency at a small CPU cost; increasing it reduces CPU overhead but allows more drift to accumulate between corrections. The fields are parsed but have no runtime effect in v3.0.0.

## Keeper WAL

These matter only for durable writes, with `chrono_keeper.WAL.enabled` set. See [Keeper WAL](../architecture/keeper-wal.md) and the [`WAL` block](./server-configuration.md#wal--keeper-write-ahead-log).

- **Where the WAL lives decides its cost.** A durable write waits for an `fdatasync` of the WAL's drive: about 0.23–0.25 ms at the median for a 1 KB event with one writer on a local NVMe drive (Debug build), and about 0.1 ms on tmpfs, which does not survive a node reboot. Put it on a local NVMe or SSD drive, not NFS or the boot disk.
- **`group_commit_window_us`** (default `0`): how long the committer waits for more events before a sync. `0` syncs as soon as the previous sync returns, which already groups every write that arrived during it; with 8 writers that reached about 15,000 durable events/s. A window helps only when writers arrive just after a sync, and it adds up to its length to a durable write's latency.
- **`group_commit_max_bytes`** (default 4 MiB): the largest group written and synced at once.
- **`durable_cap_mb`** (default `4096`): the memory of chunks the grapher has not confirmed above which durable writes get `CL_ERR_DURABILITY_FULL`. Size it for the longest grapher outage durable writers should ride out.

## Shutdown

```json
"chrono_keeper": {
  "DataStoreInternals": { "shutdown_confirm_timeout_secs": 150 }
}
```

A keeper stopped with SIGTERM waits up to `shutdown_confirm_timeout_secs` for ChronoGrapher to confirm its chunks written, then logs any chunk still unconfirmed and exits. Keep it above the grapher's `story_chunk_duration_secs` plus `acceptance_window_secs` (90 s in the template), stop the graphers only after the keepers have exited, and give a keeper more time to stop than this timeout. See [Durable Chunk Retention](../architecture/durable-retention.md#failure-behavior).

ChronoVisor's `delayed_data_admin_exit_in_secs` (template `3`) is not a data setting: it is how long the visor keeps its admin RPC client for a process that left before destroying it, and it holds no events.

## Summary of Tuning Recommendations

| Goal                                           | Recommended change                                                                            |
| ------------------------------------------------| -----------------------------------------------------------------------------------------------|
| Lower write-to-query latency                   | Reduce `story_chunk_duration_secs` on ChronoKeeper (e.g., `5`) and ChronoGrapher (e.g., `15`), keeping the [timing rules](./server-configuration.md#timing-rules-across-components) |
| Tolerate high clock skew across nodes          | Increase `acceptance_window_secs` on ChronoKeeper (e.g., `60`), and the grapher's above the keeper's `story_chunk_duration_secs` plus `acceptance_window_secs` |
| Reduce memory usage with many stories          | Decrease `inactive_story_delay_secs` on Keeper and Grapher                                    |
| Minimum timestamp latency on bare-metal HPC    | Set `clocksource_type` to `"TSC"` (verify invariant TSC first)                                |
| Keepers exit before their last chunks are written | Raise `shutdown_confirm_timeout_secs` on ChronoKeeper above the grapher's write window, and stop graphers after keepers |
| High client fan-in into ChronoKeeper           | Raise `KeeperRecordingService.IngestionThreadCount` (e.g., `8` or `16`)                       |
| Many ChronoKeepers draining one ChronoGrapher  | Raise `KeeperGrapherDrainService.IngestionThreadCount` to match concurrent drain RPCs         |
| Lower durable write latency                    | Put the keeper WAL on a local NVMe drive and keep `group_commit_window_us` at `0`             |
| Durable writes refused during a grapher outage | Raise `WAL.durable_cap_mb` on ChronoKeeper, with keeper memory to match                       |
| Extraction-bound ChronoKeeper or ChronoGrapher | Raise `ExtractionModule.extraction_stream_count` until storage/network saturates              |
