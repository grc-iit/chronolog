---
sidebar_position: 2
title: "Multi-Node Deployment"
---

# Multi-Node (Cluster) Deployment

`deploy_cluster.sh` deploys ChronoLog across multiple nodes using `parallel-ssh` for remote process management. Like `deploy_local.sh`, it operates on an already-installed ChronoLog tree and supports starting, stopping, and cleaning only — no build or install steps. The script is located at `tools/deploy/deploy_cluster.sh` in the repository.

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

## Archive on a Shared File System

ChronoGraphers write the HDF5 archive into the output directory (`-u, --output-dir`), and
ChronoPlayers on other nodes read it from the same path, so that directory must be on a file system
every grapher and player node mounts. Keepers and clients never touch it. The layout is described in
[HDF5 Archive](../architecture/hdf5-archive.md).

A player finds new files through the graphers' manifest logs in `<archive>/%manifest/`, not by
listing the story directories, so the NFS directory caches (`acdirmin`, `acdirmax`) do not decide
when a replay sees a new file. They do decide when it sees a new log: a player lists `%manifest/`
to find the logs, and that listing can be up to `acdirmax` old, so a log a grapher creates while
players run can go unseen that long.
Each grapher creates its log when it starts. Windows are aligned to `story_chunk_duration_secs`
boundaries, so its first window can be written one `acceptance_window_secs` after it starts (60 s in
the template), and keepers free those chunks `archive_visibility_delay_secs` (10 s) later. Keep
`acdirmax` below the sum, 70 s with the template; the NFS default, 60 s, is.

What the player relies on is NFS close-to-open consistency: a grapher closes its log after each
record, and a player that opens the log afterwards sees the record. That is the NFS default. A
mount with `nocto` drops it, and a player can keep reading an old copy of a log for up to
`acregmax`, while a keeper frees a chunk `archive_visibility_delay_secs` (10 s) after ChronoGrapher
confirms it written; a replay can then miss those events and still report success.

Cached failed lookups (`lookupcache`) do not matter. A client that looked a path up while it was
missing can keep answering "not found" for it for up to `acdirmax`, but no archive path is ever
removed and created again: every file gets a name used once, and a destroy deletes a story's files
but keeps its directories (see [HDF5 Archive](../architecture/hdf5-archive.md)).

Keep the clocks of the grapher hosts and the archive's file server within 120 s of each other
(NTP). A destroy deletes another grapher's files of the story by their modification time, which the
file server sets; see [HDF5 Archive](../architecture/hdf5-archive.md#destroying-a-story-or-a-chronicle).

`deploy_cluster.sh --start` checks this before it launches anything: on each ChronoPlayer host it
reads how the output directory is mounted (`findmnt`), and prints a warning naming the host when the
mount is NFS with `nocto`, or with `acdirmax` at or above the grapher's `acceptance_window_secs` plus
the keeper's `archive_visibility_delay_secs`. The deployment goes ahead either way.

Lustre keeps client caches coherent through its lock manager, so this section is specific to NFS.

## Execution Modes

Exactly one mode must be specified per invocation:

| Mode | Description |
|------|-------------|
| `--start` | Start all ChronoLog processes across the cluster |
| `--stop` | Stop all ChronoLog processes gracefully. Keepers stop first and get 4 minutes, since each waits for ChronoGrapher to confirm its chunks written; other processes are force-killed after 5 minutes. |
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
