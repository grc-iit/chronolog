---
sidebar_position: 2
title: "Server Configuration"
---

# Server Configuration File

ChronoLog server executables share a single JSON configuration file. The repository ships a template at `default_conf.json.in`; a fully expanded example is installed as `conf/default-chrono-conf.json` and is used by `tools/deploy/deploy_local.sh`.

As of ChronoLog v2.8.0 the file is organized into two shared blocks (`clock`, `authentication`) and four per-component sections (`chrono_visor`, `chrono_keeper`, `chrono_grapher`, `chrono_player`). See the [Overview](./overview.md) page for the loader architecture behind these sections.

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

### `DataStoreInternals` — story-chunk tuning

Appears in `chrono_keeper`, `chrono_grapher`, and `chrono_player`. Parsed by `DataStoreConf`. See [Performance Tuning → Story Chunk Settings](./performance-tuning.md#story-chunk-settings) for the semantics of each field.

| Field                       | Type    | Description                                                           |
| --------------------------- | ------- | --------------------------------------------------------------------- |
| `max_story_chunk_size`      | integer | Maximum number of events in a single chunk.                           |
| `story_chunk_duration_secs` | integer | How long a chunk remains open.                                        |
| `acceptance_window_secs`    | integer | Maximum allowed age of an incoming event relative to wall-clock time. |
| `inactive_story_delay_secs` | integer | Idle time before an in-memory story is evicted.                       |

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
| `hdf5_extractor`                  | grapher             | `hdf5_archive_dir` (string)                                                                 | Serializes each StoryChunk into the HDF5 archive under `hdf5_archive_dir`.                                            |

See the keeper and grapher blocks in [`conf/default_conf.json.in`](https://github.com/grc-iit/ChronoLog/blob/develop/conf/default_conf.json.in) for fully expanded examples.

### `ArchiveReaders` — story files directory

Present under `chrono_player`. Parsed by `ExtractorReaderConf`.

| Field             | Type   | Description                                                                  |
| ----------------- | ------ | ---------------------------------------------------------------------------- |
| `story_files_dir` | string | Filesystem directory where archived story files are read from by the player. |

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
