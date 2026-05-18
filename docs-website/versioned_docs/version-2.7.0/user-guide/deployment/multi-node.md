---
sidebar_position: 2
title: "Multi-Node Deployment"
---

# Multi-Node (Cluster) Deployment

`deploy_cluster.sh` deploys ChronoLog across multiple nodes using `parallel-ssh` for remote process management. Like `deploy_local.sh`, it operates on an already-installed ChronoLog tree and supports starting, stopping, and cleaning only — no build or install steps. The script is located at `tools/deploy/ChronoLog/deploy_cluster.sh` in the repository.

## Prerequisites

The following tools must be available on `PATH` of the **launch node**:

| Tool | Purpose |
|------|---------|
| `jq` | Parse and generate JSON configuration files |
| `parallel-ssh` | Launch/stop remote processes in parallel |
| `ssh` | Remote command execution and hostname resolution |
| `ldd` | Inspect shared library dependencies |
| `nohup` | Run processes independent of the shell session |
| `pkill` | Stop processes by name/path |
| `readlink` | Resolve symbolic links |
| `realpath` | Canonicalize file paths |
| `chrpath` | Adjust RPATH entries in binaries |

**Passwordless SSH** must be configured from the launch node to all cluster nodes. The ChronoLog installation tree (`<work-dir>`) must be accessible at the same path on all nodes (e.g., via a shared filesystem).

## Host Files

Without a Slurm job ID, the script reads host lists from plain-text files under `<work-dir>/conf/`, one hostname per line:

| File | Role |
|------|------|
| `hosts_visor` | Node(s) running ChronoVisor — typically one entry |
| `hosts_grapher` | Nodes running ChronoGraphers — one per recording group |
| `hosts_player` | Nodes running ChronoPlayers — one per recording group |
| `hosts_keeper` | Nodes running ChronoKeepers — all keeper nodes |

Example `hosts_keeper`:

```
node01
node02
node03
node04
```

Keepers are distributed evenly across recording groups. With 4 keepers and 2 recording groups, each group gets 2 keepers.

## Slurm Integration

When `--job-id <JOB_ID>` is provided, the script derives the node list from the active Slurm job and overwrites the host files:

- First node → ChronoVisor
- Last `N` nodes → ChronoGraphers and ChronoPlayers (one per recording group)
- All nodes → ChronoKeepers

:::important
ChronoLog must be launched from an **interactive job shell**, not via `sbatch` or `srun`. The script will exit with an error if it detects it is running inside an `srun` step.
:::

To start an interactive session first:

```bash
salloc -N 8 --time=01:00:00
# then, from the interactive shell:
./deploy_cluster.sh --start --job-id $SLURM_JOB_ID --record-groups 2
```

## Recording Groups

`--record-groups` controls how many ChronoGrapher + ChronoPlayer pairs are deployed. Each recording group handles a disjoint subset of ChronoKeepers. Increasing the number of recording groups improves write throughput for large deployments.

The value of `--record-groups` must be ≤ the total number of keeper nodes.

## Execution Modes

Exactly one mode must be specified per invocation:

| Mode | Description |
|------|-------------|
| `--start` | Start all ChronoLog processes across the cluster |
| `--stop` | Stop all ChronoLog processes gracefully (force-kills after 5 minutes) |
| `--clean` | Remove generated config files, per-group host files, logs, and output. All processes must be stopped first. |

## Options

| Option | Default | Description |
|--------|---------|-------------|
| `-w, --work-dir <path>` | Script location `/../` | Root of the installed ChronoLog tree |
| `-r, --record-groups <n>` | `1` (or count of lines in `hosts_grapher` if it exists) | Number of recording groups |
| `-j, --job-id <id>` | _(none)_ | Slurm job ID; overrides host files when set |
| `-m, --monitor-dir <path>` | `<work-dir>/monitor` | Directory for remote process launch logs |
| `-u, --output-dir <path>` | `<work-dir>/output` | Directory for story file output |
| `-v, --visor-bin <path>` | `<work-dir>/bin/chrono-visor` | Path to the ChronoVisor binary |
| `-g, --grapher-bin <path>` | `<work-dir>/bin/chrono-grapher` | Path to the ChronoGrapher binary |
| `-p, --keeper-bin <path>` | `<work-dir>/bin/chrono-keeper` | Path to the ChronoKeeper binary |
| `-a, --player-bin <path>` | `<work-dir>/bin/chrono-player` | Path to the ChronoPlayer binary |
| `-f, --conf-file <path>` | `<work-dir>/conf/default-chrono-conf.json` | Main configuration template |
| `-n, --client-conf-file <path>` | `<work-dir>/conf/default-chrono-client-conf.json` | Client configuration template |
| `-q, --visor-hosts <path>` | `<work-dir>/conf/hosts_visor` | Override path to visor host file |
| `-k, --grapher-hosts <path>` | `<work-dir>/conf/hosts_grapher` | Override path to grapher host file |
| `-o, --keeper-hosts <path>` | `<work-dir>/conf/hosts_keeper` | Override path to keeper host file |
| `-e, --verbose` | `false` | Enable verbose output |

## Examples

Start with pre-configured host files (default work-dir):

```bash
./deploy_cluster.sh --start
```

Start using a Slurm job with 2 recording groups:

```bash
./deploy_cluster.sh --start --job-id $SLURM_JOB_ID --record-groups 2
```

Start from a custom installation with 3 recording groups:

```bash
./deploy_cluster.sh --start --work-dir /shared/chronolog --record-groups 3
```

Stop the deployment using existing host files:

```bash
./deploy_cluster.sh --stop
```

Stop using a Slurm job (re-derives hosts from job):

```bash
./deploy_cluster.sh --stop --job-id $SLURM_JOB_ID
```

Clean up generated files (run after stopping):

```bash
./deploy_cluster.sh --clean
```
