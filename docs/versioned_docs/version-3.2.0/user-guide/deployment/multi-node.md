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
every grapher and player node mounts. Keepers and clients never touch it.

On NFS, two client-side caches decide how soon a player sees a file a grapher on another node has
just written:

| Cache | Mount options | Effect on a replay |
|-------|---------------|--------------------|
| Failed lookups | `lookupcache` (the default, `all`, caches them) | A player that looked a file name up before the file existed keeps getting "not found" for it until it revalidates the directory, which can take up to `acdirmax`. A keeper frees a chunk `archive_visibility_delay_secs` (10 s) after ChronoGrapher confirms it written, so a longer miss leaves those events out of a replay that still reports success. |
| Directory attributes and listings | `acdirmin`, `acdirmax` (30 s and 60 s by default) | The player's directory listing, repeated every `archive_scan_interval_secs`, can be up to `acdirmax` old. |

File attribute caching (`acregmin`, `acregmax`) does not matter here: an archive file never changes
once it appears under its name. Lustre keeps client metadata caches coherent through its lock
manager, so this section is specific to NFS.

The recommended setup is a mount of the archive directory alone, on the grapher and player nodes:

```
server:/export/chronolog-archive  /mnt/chronolog-archive  nfs  lookupcache=positive,acdirmin=3,acdirmax=5,nosharecache,<site options>  0 0
```

| Option | Why |
|--------|-----|
| `lookupcache=positive` | Failed lookups are not cached, so a player finds a file on its next lookup. Found files are still cached. |
| `acdirmin=3,acdirmax=5` | A directory listing is at most about 5 seconds old. |
| `nosharecache` | Needed when this mount comes from the same export as another mount with different options; otherwise the client shares one cache between them and may keep the other mount's options. |

The cost is one server lookup for each missing name a player asks for, and one attribute check of
the archive directory every few seconds, from the grapher and player nodes only. A site that keeps
long attribute caches on its general mount to spare the server, for instance a RAID array of hard
drives serving many compute nodes, can leave that mount as it is. Then pass the new mount point to
the deploy script, which sets it as both the graphers' `hdf5_archive_dir` and the players'
`story_files_dir`:

```bash
./deploy_cluster.sh --start -u /mnt/chronolog-archive
```

If the archive has to stay on a mount that caches failed lookups, raise
`archive_visibility_delay_secs` above that mount's `acdirmax` instead. This costs keeper memory: each
keeper holds every chunk that much longer after it is written.

To measure a mount, cache a failed lookup on one node and time how long it takes to see a file
created on another:

```bash
# node A (a player node)
f=/mnt/chronolog-archive/lookup_probe_$$; stat "$f" 2>/dev/null; echo "$f"
while ! stat "$f" >/dev/null 2>&1; do sleep 1; done; date +%T
# node B (a grapher node), a few seconds later, with the name node A printed
touch <that name>; date +%T
```

The gap between the two times is how long the mount keeps a failed lookup. The same loop with `ls`
in place of `stat` measures how old a directory listing can be.

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
