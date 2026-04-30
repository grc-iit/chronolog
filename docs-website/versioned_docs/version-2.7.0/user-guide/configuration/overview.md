---
sidebar_position: 1
title: "Overview"
---

# Using Configuration Files

ChronoLog is configured through a single JSON file passed to every server executable via the `--config` flag:

```bash
chrono-visor    --config default_conf.json
chrono-keeper   --config default_conf.json
chrono-grapher  --config default_conf.json
chrono-player   --config default_conf.json
```

The same file can be shared across all server processes: each executable only reads the sections that belong to it and ignores the rest. Client applications use a separate, smaller configuration file described in [Client Configuration](./client-configuration.md).

## Configuration Layout

The server configuration is organized into two groups of top-level keys:

- **Shared blocks** — `clock`, `authentication`. Their semantics are identical for every ChronoLog process.
- **Per-component sections** — `chrono_visor`, `chrono_keeper`, `chrono_grapher`, `chrono_player`. Each process reads only its own section.

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

See [Server Configuration](./server-configuration.md) for the full field reference.

## How the Configuration is Loaded

Starting with ChronoLog v2.7.0 the configuration system is split into a lightweight loader and a set of per-component configuration classes. This replaces the earlier monolithic `ConfigurationManager` that owned the parsed state of every component.

### `ConfigurationManager` (shared loader)

`chrono_common/include/ConfigurationManager.h` parses the JSON file once and exposes:

- `ClockConf CLOCK_CONF` — clock-source settings parsed from `clock`.
- `AuthConf AUTH_CONF` — authentication settings parsed from `authentication`.
- `json_object* VISOR_JSON_CONF`, `KEEPER_JSON_CONF`, `GRAPHER_JSON_CONF`, `PLAYER_JSON_CONF` — unparsed JSON subtrees for each component.

The loader does **not** know the fields of any component. It simply hands the relevant JSON subtree to whichever process asked for it:

```cpp
chronolog::ConfigurationManager confManager(conf_file_path);
// confManager.CLOCK_CONF and confManager.AUTH_CONF are ready to use.
// confManager.VISOR_JSON_CONF / KEEPER_JSON_CONF / ... hold the raw JSON
// subtree for each component — only the process that needs it will parse it.
```

### Per-Component Configuration Classes

Each server process owns a configuration class that lives alongside its source tree and parses only the JSON subtree handed to it:

| Process        | Header                                                | Class                  |
| -------------- | ----------------------------------------------------- | ---------------------- |
| ChronoVisor    | `ChronoVisor/include/ChronoVisorConfiguration.h`      | `VisorConfiguration`   |
| ChronoKeeper   | `ChronoKeeper/include/ChronoKeeperConfiguration.h`    | `KeeperConfiguration`  |
| ChronoGrapher  | `ChronoGrapher/include/ChronoGrapherConfiguration.h`  | `GrapherConfiguration` |
| ChronoPlayer   | `ChronoPlayer/include/ChronoPlayerConfiguration.h`    | `PlayerConfiguration`  |

A typical startup sequence inside each server executable looks like:

```cpp
chronolog::ConfigurationManager confManager(conf_file_path);

chronolog::VisorConfiguration visorConf; // or KeeperConfiguration, etc.
if (visorConf.parseJsonConf(confManager.VISOR_JSON_CONF) != chronolog::CL_SUCCESS)
{
    // bail out
}
```

### Shared Configuration Blocks

Configuration fragments that appear in more than one component are defined once in `chrono_common/include/ConfigurationBlocks.h` and composed into each per-component class:

| Block                 | Purpose                                                                |
| --------------------- | ---------------------------------------------------------------------- |
| `ClockConf`           | Clock source and drift calibration parameters (`clock` block).         |
| `AuthConf`            | Authentication type and module path (`authentication` block).          |
| `RPCProviderConf`     | One Thallium endpoint: protocol, IP, port, provider id (`rpc` block).  |
| `LogConf`             | Per-process log sink configuration (`Monitoring.monitor` block).       |
| `DataStoreConf`       | Story-chunk sizing and lifecycle (`DataStoreInternals` block).         |
| `ExtractorReaderConf` | Filesystem location for story files (`Extractors` / `ArchiveReaders`). |

### Why the Split Matters

- **Independent evolution.** A change to Keeper-specific fields no longer forces a rebuild of ChronoVisor, ChronoGrapher, and ChronoPlayer.
- **Smaller blast radius.** Each process parses and validates only the section it cares about; an unknown field inside `chrono_keeper` cannot affect ChronoVisor start-up.
- **Foundation for dynamic reload.** Because `ConfigurationManager` no longer owns per-component state, it can re-load the JSON on signal without touching the running component configurations.

This split was introduced in [PR #564](https://github.com/grc-iit/ChronoLog/pull/564) as preparation for the upcoming extractor-module sections inside the ChronoKeeper configuration.
