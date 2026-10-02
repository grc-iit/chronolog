---
sidebar_position: 2
title: "Server Configuration"
---

# Server Configuration File

ChronoLog server executables share a single JSON configuration file. The repository ships a template at `default_conf.json.in`; a fully expanded example is installed as `conf/default-chrono-conf.json` and is used by `tools/deploy/deploy_local.sh`.

The file is organized into two shared blocks (`clock`, `authentication`) and four per-component sections (`chrono_visor`, `chrono_keeper`, `chrono_grapher`, `chrono_player`). See the [Overview](./overview.md) page for the loader architecture behind these sections.

```json
{
  "clock":          { ... },
  "authentication": { ... },
  "chrono_visor":   { ... },
  "chrono_keeper":  { ... },
  "chrono_grapher": { ... },
  "chrono_player":  { ... }
}
```

---

## Shared Blocks

These blocks are parsed once by `ConfigurationManager` and are visible to every process.

### `clock`

Controls the clock source used for event timestamping. See [Performance Tuning → Clock Source](./performance-tuning.md#clock-source) for the effect of each option.

| Field                  | Type    | Default       | Description                                                    |
| ---------------------- | ------- | ------------- | -------------------------------------------------------------- |
| `clocksource_type`     | string  | `"CPP_STYLE"` | One of `"C_STYLE"`, `"CPP_STYLE"`, `"TSC"`.                    |
| `drift_cal_sleep_sec`  | integer | `10`          | Seconds between clock drift calibrations.                      |
| `drift_cal_sleep_nsec` | integer | `0`           | Additional nanoseconds added to the calibration interval.      |

Backed by `ClockConf` in `src/chrono-common/include/ConfigurationBlocks.h`.

### `authentication`

| Field             | Type   | Default  | Description                                                |
| ----------------- | ------ | -------- | ---------------------------------------------------------- |
| `auth_type`       | string | `"RBAC"` | Authentication mechanism. Currently only `"RBAC"` is used. |
| `module_location` | string | `""`     | Path to the authentication module shared object.           |

Backed by `AuthConf` in `src/chrono-common/include/ConfigurationBlocks.h`.

---

## Common Per-Component Sub-Blocks

Several JSON sub-blocks appear in more than one component. They always have the same shape, parsed by the same C++ building block.

### `rpc` — Thallium endpoint

Every service endpoint is described by an `rpc` block, parsed by `RPCProviderConf`.

| Field                 | Type    | Example         | Description                                                                   |
| --------------------- | ------- | --------------- | ----------------------------------------------------------------------------- |
| `protocol_conf`       | string  | `"ofi+sockets"` | Thallium/Mercury transport string. See [Network & RPC](./network-and-rpc.md). |
| `service_ip`          | string  | `"127.0.0.1"`   | IP the service binds to (servers) or connects to (clients).                   |
| `service_base_port`   | integer | `5555`          | TCP port.                                                                     |
| `service_provider_id` | integer | `55`            | Thallium provider ID; multiplexes services on the same address.               |

### `Monitoring` — logging

Per-component log sink. Wrapped under a `monitor` key and parsed by `LogConf`.

```json
"Monitoring": {
  "monitor": {
    "type": "file",
    "file": "chrono-visor-1.log",
    "level": "debug",
    "name": "ChronoVisor",
    "filesize": 104857600,
    "filenum": 3,
    "flushlevel": "warning"
  }
}
```

| Field        | Type    | Description                                                                         |
| ------------ | ------- | ----------------------------------------------------------------------------------- |
| `type`       | string  | Log sink type. Currently only `"file"` is supported.                                |
| `file`       | string  | Log file name.                                                                      |
| `level`      | string  | Minimum log level: `trace`, `debug`, `info`, `warning`, `error`, `critical`, `off`. |
| `name`       | string  | Logger name that appears in log lines.                                              |
| `filesize`   | integer | Maximum file size in bytes before rotation.                                         |
| `filenum`    | integer | Number of rotated files to keep.                                                    |
| `flushlevel` | string  | Minimum level that triggers an immediate flush to disk.                             |

### `DataStoreInternals` — story-chunk, tail and retention tuning {#datastoreinternals--story-chunk-tuning}

Appears in `chrono_keeper`, `chrono_grapher`, and `chrono_player`. Parsed by `DataStoreConf`. See [Performance Tuning → Story Chunk Settings](./performance-tuning.md#story-chunk-settings) for the semantics of each field.

| Field                       | Type    | Default | Description                                                           |
| --------------------------- | ------- | ------- | --------------------------------------------------------------------- |
| `max_story_chunk_size`      | integer | `64`    | Maximum number of events in a single chunk.                           |
| `story_chunk_duration_secs` | integer | `30`    | How long a chunk remains open.                                        |
| `acceptance_window_secs`    | integer | `60`    | Maximum allowed age of an incoming event relative to wall-clock time. |
| `inactive_story_delay_secs` | integer | `180`   | Idle time before an in-memory story is evicted.                       |
| `tail_capacity`             | integer | `65536` | *(Keeper only)* Maximum most-recent sealed events indexed per story for tail reads. A chunk with events in the index stays in memory; the index is released when the story retires. Must be $>0$. |
| `live_tail_read`            | boolean | `false` | *(Keeper only)* When true, tail reads also serve unsealed events from the active timeline in addition to sealed chunks, dropping visibility latency to sub-second. |
| `retention_cap_mb`          | integer | `4096`  | *(Keeper only)* Retained-chunk memory, in MB, above which the keeper logs a warning while it waits for ChronoGrapher to persist chunks. Nothing is dropped. Set it above the memory a healthy keeper holds, or the warning fires in normal operation: about `3 × 125 s × peak per-keeper MB/s` with the template's windows. `0` turns the warning off. See [Durable Chunk Retention](../architecture/durable-retention.md). |
| `watermark_resend_timeout_secs` | integer | `300` | *(Keeper only)* Seconds a keeper waits for ChronoGrapher to confirm a chunk written before sending it again. Keep it at least twice ChronoGrapher's `story_chunk_duration_secs` plus `acceptance_window_secs`; see [Timing rules across components](#timing-rules-across-components). Must be positive. |
| `archive_visibility_delay_secs` | integer | `10` | *(Keeper only)* Seconds a keeper keeps a chunk after ChronoGrapher confirms it written, so replays take its events from the keeper until players can read the new archive file. A replay looks the newest windows up by name, which skips a shared file system's directory listing cache but not its cache of failed lookups, which NFS keeps by default (`lookupcache=all`) for up to `acdirmax`: mount the archive with `lookupcache=positive`, or raise this above `acdirmax`. A window written late at an older start time, after a lost chunk was sent again, is found only by the player's directory scan, so for those this has to cover `archive_scan_interval_secs` plus the listing cache; see [Timing rules across components](#timing-rules-across-components) and [Archive on a Shared File System](../deployment/multi-node.md#archive-on-a-shared-file-system). `0` frees on confirmation. See [Durable Chunk Retention](../architecture/durable-retention.md). |
| `shutdown_confirm_timeout_secs` | integer | `150` | *(Keeper only)* Seconds a keeper stopped with SIGTERM waits for ChronoGrapher to confirm its chunks written before it exits. Cover ChronoGrapher's `story_chunk_duration_secs` plus `acceptance_window_secs`; see [Timing rules across components](#timing-rules-across-components). `0` exits without waiting. |
| `watermark_report_interval_secs` | integer | `1` | *(Grapher only)* How often ChronoGrapher sends changed persisted watermarks and unwritten receipts to the keepers. |


### Timing rules across components

Several timing settings only work together with a setting of another component: a keeper waits for
ChronoGrapher to write its chunks, and a player has to find a file before the keeper lets its copy
go. Each component reads only its own settings, so nothing checks these relations when a component
starts. If you change any of the settings below, keep the rules. The values are the template's.

| Rule | Template values | Why | If it is broken |
| --- | --- | --- | --- |
| player `archive_window_secs` = grapher `story_chunk_duration_secs` | 30 = 30 | The player builds the names of the newest archive files from the grapher's window length. | Lookups by name never match; new files are found only by the next directory scan, and a replay in between can miss events the keepers have freed. |
| grapher `acceptance_window_secs` > keeper `story_chunk_duration_secs` + keeper `acceptance_window_secs` | 60 > 10 + 15 | A keeper ships a chunk that long after its first event; it has to reach the grapher before the grapher's window for that time closes. | Every keeper chunk arrives after its window was written and lands in a second, numbered file for that window. |
| grapher `story_chunk_duration_secs` + `acceptance_window_secs` (the write window) stays short | 30 + 60 = 90 s | An event reaches the archive within the write window, and a keeper holds each chunk at least that long. | Keepers hold more memory and replays read more from the keepers. |
| keeper `watermark_resend_timeout_secs` ≥ 2 × the grapher's write window | 300 ≥ 180 | A keeper sends a chunk again only when the grapher has had time to write it. | A delivery about to be confirmed is replaced by a new one that has to be confirmed from the start; chunks are sent and written again, and a keeper can wait indefinitely. |
| keeper `shutdown_confirm_timeout_secs` ≥ the grapher's write window | 150 ≥ 90 | A keeper stopped with SIGTERM waits for the grapher to write the chunks it just sent. | The keeper exits before its last chunks are confirmed, and frees them unconfirmed with a warning. |
| keeper `archive_visibility_delay_secs` ≥ player `archive_scan_interval_secs` + the archive mount's `acdirmax` | 10 vs 5 + 5 with the recommended mount | A window written late at an older start time is found only by the player's directory scan, which lags by the mount's directory cache. | A replay issued between the keeper freeing such a chunk and the next listing that shows its file misses those events and still reports success. |
| keeper `retention_cap_mb` ≥ 3 × peak per-keeper MB/s × (keeper `story_chunk_duration_secs` + `acceptance_window_secs` + the grapher's write window + grapher `watermark_report_interval_secs` + keeper `archive_visibility_delay_secs`) | 4096 ≥ 3 × 10 × 126 | That is how long a healthy keeper holds each chunk; the factor 3 leaves room for a short grapher stall. | The retention warning fires in normal operation and stops meaning anything. Nothing is dropped. |
| grapher `watermark_report_interval_secs` well below keeper `archive_visibility_delay_secs` | 1 vs 10 | Keepers learn that a chunk is written from these reports. | Chunks stay on the keepers longer than they need to. |
| grapher `inactive_story_delay_secs` > keeper `story_chunk_duration_secs` + keeper `acceptance_window_secs` | 300 > 25 | After a story is released, a keeper seals its last chunk that long later; the grapher's pipeline for the story should still be there. | The last chunks arrive after the pipeline retired and are written as reopened windows. |

It also helps to make the grapher's `story_chunk_duration_secs` a multiple of the keeper's (30 and
10), so that a keeper's chunk does not straddle two grapher windows.

A client waits up to 180 s for a replay (fixed in the client library). A player gives each keeper
5 s to answer before it returns a partial result, so this leaves the archive read most of that time.

### `ExtractionModule`

Present under `chrono_keeper` and `chrono_grapher`. Parsed by `ExtractionModuleConfiguration` (`src/chrono-common/include/ExtractionModuleConfiguration.h`). Defines the configurable pipeline of extractors that drains retired StoryChunks. See [Architecture → ChronoKeeper](../architecture/chronokeeper.md) for the concept and [Performance Tuning → Extraction Pipeline](./performance-tuning.md#extraction-pipeline) for tuning guidance.

| Field                     | Type    | Description                                                                                                                                                                       |
| ------------------------- | ------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `extraction_stream_count` | integer | Number of parallel extraction-stream threads draining the extraction queue. Default: `2`.                                                                                         |
| `extraction_protocol`     | string  | Thallium protocol used by RDMA-based extractors. Default: `"ofi+sockets"`.                                                                                                        |
| `extractors`              | object  | Map of extractor instances keyed by an arbitrary label. Each entry must set `"type"` to one of the supported extractor types listed below; additional fields depend on the type.  |

**Supported extractor types**

| `type`                            | Available in        | Required fields                                                                            | Behavior                                                                                                              |
| --------------------------------- | ------------------- | ------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------- |
| `csv_extractor`                   | keeper, grapher     | `csv_archive_dir` (string)                                                                 | Writes each StoryChunk as a CSV file under `csv_archive_dir`.                                                         |
| `single_endpoint_rdma_extractor`  | keeper, grapher     | `receiving_endpoint` (object: `protocol_conf`, `service_ip`, `service_base_port`, `service_provider_id`) | Drains each StoryChunk to one RDMA endpoint (typically a ChronoGrapher's `KeeperGrapherDrainService`).                |
| `dual_endpoint_rdma_extractor`    | keeper              | Two `receiving_endpoint` entries                                                            | Fans each StoryChunk out to two RDMA endpoints simultaneously (e.g. ChronoGrapher + ChronoPlayer).                    |
| `hdf5_extractor`                  | grapher (required)  | `hdf5_archive_dir` (string)                                                                 | Serializes each StoryChunk into the HDF5 archive under `hdf5_archive_dir`.                                            |

A ChronoGrapher's chain must include `hdf5_extractor`: it is the only extractor that confirms chunks written, and keepers free a chunk only once the grapher confirms it. A grapher whose `extractors` list is empty or absent gets an `hdf5_extractor` with `hdf5_archive_dir` `/tmp`; one whose list leaves it out, such as a `csv_extractor` alone, fails to start. Other extractors may run alongside it.

Keepers ship with `single_endpoint_rdma_extractor` to ChronoGrapher. Replay reads recent events from the keepers directly, so the ChronoPlayer copy that `dual_endpoint_rdma_extractor` sends is no longer needed.

See the keeper and grapher blocks in [`conf/default_conf.json.in`](https://github.com/grc-iit/ChronoLog/blob/develop/conf/default_conf.json.in) for fully expanded examples.

### `ArchiveReaders` — story files directory

Present under `chrono_player`. Parsed by `ExtractorReaderConf`.

| Field             | Type   | Description                                                                  |
| ----------------- | ------ | ---------------------------------------------------------------------------- |
| `story_files_dir` | string | Filesystem directory where archived story files are read from by the player. On NFS, mount it with `lookupcache=positive` and a short `acdirmin`/`acdirmax`; see [Archive on a Shared File System](../deployment/multi-node.md#archive-on-a-shared-file-system). |
| `archive_scan_interval_secs` | integer | Seconds between the player's listings of `story_files_dir`. A file written since the last listing is found by a lookup by name if it is one of the newest windows, and otherwise only by the next listing. Default: `5`. |
| `archive_window_secs` | integer | Length of ChronoGrapher's windows, used to build the names of the newest archive files for a lookup by name. Must equal the grapher's `story_chunk_duration_secs`. `0` turns the lookup off. Default: `30`. |

### `IngestionThreadCount` — ingestion-thread parallelism

A scalar field that sits inside the ingestion service object of each component:

| Component      | Path                                                          | Default | Description                                                                  |
| -------------- | ------------------------------------------------------------- | ------- | ---------------------------------------------------------------------------- |
| ChronoKeeper   | `chrono_keeper.KeeperRecordingService.IngestionThreadCount`   | `4`     | Number of worker threads serving the Keeper recording RPC.                   |
| ChronoGrapher  | `chrono_grapher.KeeperGrapherDrainService.IngestionThreadCount` | `1`   | Number of worker threads serving the Grapher drain RPC.                      |
| ChronoPlayer   | `chrono_player.PlaybackQueryService.IngestionThreadCount`     | `1`     | Number of worker threads serving the Player playback-query RPC.              |

---

## `chrono_visor`

Parsed by `VisorConfiguration` (`src/chrono-visor/include/ChronoVisorConfiguration.h`).

| Field                             | Type    | Description                                                                                   |
| --------------------------------- | ------- | --------------------------------------------------------------------------------------------- |
| `VisorClientPortalService`        | object  | `{ "rpc": { ... } }` — endpoint used by clients to connect to ChronoVisor.                    |
| `VisorKeeperRegistryService`      | object  | `{ "rpc": { ... } }` — endpoint where ChronoKeeper/Grapher/Player register and heartbeat.     |
| `Monitoring`                      | object  | See [`Monitoring`](#monitoring--logging).                                                     |
| `delayed_data_admin_exit_in_secs` | integer | Shutdown grace period for the data administration service. Clamped to `(0, 60)`; default `5`. |

## `chrono_keeper`

Parsed by `KeeperConfiguration` (`src/chrono-keeper/include/ChronoKeeperConfiguration.h`).

| Field                         | Type    | Description                                                                                |
| ----------------------------- | ------- | ------------------------------------------------------------------------------------------ |
| `RecordingGroup`              | integer | Logical group this keeper belongs to. Must match its paired ChronoGrapher.                 |
| `KeeperRecordingService`      | object  | `{ "rpc": { ... } }` — endpoint that receives events from ChronoVisor for recording.       |
| `KeeperDataStoreAdminService` | object  | `{ "rpc": { ... } }` — endpoint for admin actions on the keeper's data store.              |
| `VisorKeeperRegistryService`  | object  | `{ "rpc": { ... } }` — endpoint **on ChronoVisor** where this keeper registers.            |
| `KeeperGrapherDrainService`   | object  | `{ "rpc": { ... } }` — endpoint **on ChronoGrapher** where drained chunks are delivered.   |
| `Monitoring`                  | object  | See [`Monitoring`](#monitoring--logging).                                                  |
| `DataStoreInternals`          | object  | See [`DataStoreInternals`](#datastoreinternals--story-chunk-tuning).                       |
| `ExtractionModule`            | object  | See [`ExtractionModule`](#extractionmodule).                                               |

`KeeperRecordingService` carries an `IngestionThreadCount` field — see [`IngestionThreadCount`](#ingestionthreadcount--ingestion-thread-parallelism).

## `chrono_grapher`

Parsed by `GrapherConfiguration` (`src/chrono-grapher/include/ChronoGrapherConfiguration.h`).

| Field                       | Type    | Description                                                                                |
| --------------------------- | ------- | ------------------------------------------------------------------------------------------ |
| `RecordingGroup`            | integer | Logical group this grapher serves.                                                         |
| `KeeperGrapherDrainService` | object  | `{ "rpc": { ... } }` — endpoint where this grapher receives drained chunks from a keeper.  |
| `DataStoreAdminService`     | object  | `{ "rpc": { ... } }` — endpoint for admin actions on the grapher's data store.             |
| `VisorRegistryService`      | object  | `{ "rpc": { ... } }` — endpoint **on ChronoVisor** where this grapher registers.           |
| `Monitoring`                | object  | See [`Monitoring`](#monitoring--logging).                                                  |
| `DataStoreInternals`        | object  | See [`DataStoreInternals`](#datastoreinternals--story-chunk-tuning).                       |
| `ExtractionModule`          | object  | See [`ExtractionModule`](#extractionmodule).                                               |

`KeeperGrapherDrainService` carries an `IngestionThreadCount` field — see [`IngestionThreadCount`](#ingestionthreadcount--ingestion-thread-parallelism).

## `chrono_player`

Parsed by `PlayerConfiguration` (`src/chrono-player/include/ChronoPlayerConfiguration.h`).

| Field                     | Type    | Description                                                                                |
| ------------------------- | ------- | ------------------------------------------------------------------------------------------ |
| `RecordingGroup`          | integer | Logical group this player serves.                                                          |
| `PlayerStoreAdminService` | object  | `{ "rpc": { ... } }` — endpoint for admin actions on the player's data store.              |
| `PlaybackQueryService`    | object  | `{ "rpc": { ... } }` — endpoint that receives playback queries from clients.               |
| `VisorRegistryService`    | object  | `{ "rpc": { ... } }` — endpoint **on ChronoVisor** where this player registers.            |
| `Monitoring`              | object  | See [`Monitoring`](#monitoring--logging).                                                  |
| `DataStoreInternals`      | object  | See [`DataStoreInternals`](#datastoreinternals--story-chunk-tuning).                       |
| `ArchiveReaders`          | object  | See [`ArchiveReaders`](#archivereaders--story-files-directory).                            |

`PlaybackQueryService` carries an `IngestionThreadCount` field — see [`IngestionThreadCount`](#ingestionthreadcount--ingestion-thread-parallelism).

---

## Full Example

The repository ships a fully expanded example at [`conf/default_conf.json.in`](https://github.com/grc-iit/ChronoLog/blob/develop/conf/default_conf.json.in). Default values for every field are defined in the constructors of the configuration classes listed above.
