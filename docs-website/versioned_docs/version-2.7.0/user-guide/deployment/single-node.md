---
sidebar_position: 1
title: "Single Node Deployment"
---

# Single Node Deployment

`deploy_local.sh` deploys ChronoLog on a single machine from an already-installed ChronoLog tree. It handles starting, stopping, and cleaning only — it does not build or install. The script is located at `tools/deploy/ChronoLog/deploy_local.sh` in the repository, and is also copied into the installed tree at `<work-dir>/deploy/deploy_local.sh`.

## Prerequisites

The following tools must be available on `PATH`:

| Tool | Purpose |
|------|---------|
| `jq` | Parse and generate JSON configuration files |
| `ldd` | Inspect shared library dependencies |
| `nohup` | Run processes independent of the shell session |
| `pkill` | Stop processes by name/path |
| `readlink` | Resolve symbolic links |
| `realpath` | Canonicalize file paths |
| `chrpath` | Adjust RPATH entries in binaries |

## Directory Layout

The script operates on a ChronoLog installation tree with the following structure:

```
<work-dir>/
├── bin/          # ChronoLog binaries (chrono-visor, chrono-keeper, …)
├── lib/          # Shared libraries
├── conf/         # Configuration files (default-chrono-conf.json, …)
├── monitor/      # Log output (created automatically)
└── output/       # Story file output (created automatically)
```

If `--work-dir` is not specified, the script first looks relative to its own location, then falls back to `$HOME/chronolog-install/chronolog`.

## Configuration Files

The script expects two template configuration files under `<work-dir>/conf/`:

- `default-chrono-conf.json` — main configuration for all server components
- `default-chrono-client-conf.json` — configuration for ChronoLog clients

At startup, per-component configuration files are generated from these templates (e.g., `chrono-keeper-conf-1.json`, `chrono-grapher-conf-1.json`, etc.) and written back into `conf/`. These generated files are removed by `--clean`.

## Execution Modes

Exactly one mode must be specified per invocation:

| Mode | Description |
|------|-------------|
| `--start` | Start all ChronoLog processes |
| `--stop` | Stop all ChronoLog processes gracefully (force-kills after timeout) |
| `--clean` | Remove generated config files, logs, and output artifacts. Requires all processes to be stopped first. |

## Options

| Option | Default | Description |
|--------|---------|-------------|
| `-w, --work-dir <path>` | Script location `/../` or `$HOME/chronolog-install/chronolog` | Root of the installed ChronoLog tree |
| `-k, --keepers <n>` | `1` | Total number of ChronoKeeper processes |
| `-r, --record-groups <n>` | `1` | Number of recording groups (one ChronoGrapher + one ChronoPlayer per group) |
| `-m, --monitor-dir <path>` | `<work-dir>/monitor` | Directory for process launch logs |
| `-u, --output-dir <path>` | `<work-dir>/output` | Directory for story file output |
| `-v, --visor-bin <path>` | `<work-dir>/bin/chrono-visor` | Path to the ChronoVisor binary |
| `-g, --grapher-bin <path>` | `<work-dir>/bin/chrono-grapher` | Path to the ChronoGrapher binary |
| `-p, --keeper-bin <path>` | `<work-dir>/bin/chrono-keeper` | Path to the ChronoKeeper binary |
| `-a, --player-bin <path>` | `<work-dir>/bin/chrono-player` | Path to the ChronoPlayer binary |
| `-f, --conf-file <path>` | `<work-dir>/conf/default-chrono-conf.json` | Main configuration template |
| `-n, --client-conf-file <path>` | `<work-dir>/conf/default-chrono-client-conf.json` | Client configuration template |

## Process Startup Order

When `--start` is invoked, processes are launched in the following order, with 2-second delays between groups:

1. **ChronoVisor** — the central coordinator
2. **ChronoGraphers** — one per recording group (after 2 s)
3. **ChronoPlayers** — one per recording group (after 2 s)
4. **ChronoKeepers** — distributed evenly across recording groups (after 2 s)

All processes are launched with `nohup` and their output is written to `<monitor-dir>/`.

## Examples

Start with default settings (auto-detects installation):

```bash
./deploy_local.sh --start
```

Start with 5 keepers and 2 recording groups:

```bash
./deploy_local.sh --start --keepers 5 --record-groups 2
```

Start from a custom installation directory:

```bash
./deploy_local.sh --start --work-dir /path/to/install
```

Stop the deployment:

```bash
./deploy_local.sh --stop
```

Clean up generated files (run after stopping):

```bash
./deploy_local.sh --clean
```
