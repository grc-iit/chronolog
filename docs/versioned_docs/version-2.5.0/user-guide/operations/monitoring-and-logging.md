---
sidebar_position: 1
title: "Monitoring & Logging"
---

# Monitoring & Logging

Every ChronoLog component uses the same spdlog-based logger (`chrono_monitor`). Configuration lives in a `Monitoring.monitor` block inside each component's JSON configuration file.

## Log Configuration

Each component config contains a block like the following:

```json
"Monitoring": {
  "monitor": {
    "type": "file",
    "file": "chrono-keeper-1.log",
    "level": "debug",
    "name": "ChronoKeeper",
    "filesize": 1048576,
    "filenum": 3,
    "flushlevel": "warning"
  }
}
```

| Field | Type | Description |
|---|---|---|
| `type` | string | `"file"` writes to a rotating log file; `"console"` writes to stdout/stderr. |
| `file` | string | Path (or filename) of the log file. Relative paths are resolved from the working directory. |
| `level` | string | Minimum severity to record. One of `trace`, `debug`, `info`, `warning`, `error`, `critical`. |
| `name` | string | Logger name prefix that appears in each log line. |
| `filesize` | integer | Maximum log file size in bytes before rotation. |
| `filenum` | integer | Number of rotated backup files to keep. Older files are discarded. |
| `flushlevel` | string | Minimum severity that triggers an immediate disk flush. Messages below this level are buffered. |

### Log levels

| Level | When to use |
|---|---|
| `trace` | Extremely verbose; disabled in release builds (`NDEBUG`). |
| `debug` | Per-event or per-RPC details; disabled in release builds. |
| `info` | Normal operational milestones (service start, chronicle created, …). |
| `warning` | Recoverable conditions worth attention. |
| `error` | Failed operations that the system handled. |
| `critical` | Unrecoverable failures; component may shut down. |

:::caution
`trace` and `debug` messages are compiled out when ChronoLog is built with `NDEBUG` (Release builds). Setting `level` to `debug` in a Release build has no effect.
:::

## Per-Component Log Files

The deploy scripts write each component's structured log under `<work-dir>/monitor/`. Default file names from `default_conf.json`:

| Component | Default log file | Notes |
|---|---|---|
| ChronoVisor | `chrono-visor-1.log` | Single instance. Default `filesize`: 100 MB. |
| ChronoKeeper | `chrono-keeper-<N>.log` | One file per Keeper instance (N = 1, 2, …). Default `filesize`: 1 MB. |
| ChronoGrapher | `chrono-grapher-<N>.log` | One file per Grapher instance. Default `filesize`: 1 MB. |
| ChronoPlayer | `chrono-player-<N>.log` | One file per Player instance. Default `filesize`: 1 MB. |
| ChronoClient | `chrono-client.log` | Set in the client config file. |

In addition to the structured logs, process launch stdout/stderr is captured separately:

| Process | Launch log |
|---|---|
| ChronoVisor | `<monitor-dir>/chrono-visor.launch.log` |
| ChronoGrapher N | `<monitor-dir>/chrono-grapher-<N>.launch.log` |
| ChronoPlayer N | `<monitor-dir>/chrono-player-<N>.launch.log` |
| ChronoKeeper N | `<monitor-dir>/chrono-keeper-<N>.launch.log` |

Launch logs capture early startup errors (missing libraries, bad config path, argument parsing) that occur before the logger itself is initialised.

## Log Directory

When using the provided deploy scripts the monitor directory defaults to `<work-dir>/monitor/`. It is created automatically on `--start`. You can override it with the `--monitor-dir` flag:

```bash
./deploy_local.sh --start --monitor-dir /var/log/chronolog
```

## Health Checks

ChronoVisor continuously monitors registered service nodes. It uses the `collection_service_available()` RPC as an internal liveness probe before issuing administrative operations (e.g., data drain, shutdown sequencing) to Keepers, Graphers, and Players. There is no separate health-check endpoint exposed externally; if a component is unreachable its registration is treated as unavailable.

To verify the system is up from outside, check that the expected processes are running and that the Visor port (default `5555`) accepts connections:

```bash
pgrep -a chrono
ss -tlnp | grep 5555
```

## Log Level Recommendations

| Environment | Recommended `level` | Recommended `flushlevel` |
|---|---|---|
| Production | `info` or `warning` | `warning` |
| Debugging / development | `debug` | `debug` |
| Performance benchmarking | `warning` | `error` |
