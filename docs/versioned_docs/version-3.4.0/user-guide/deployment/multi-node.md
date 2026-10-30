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

## Keeper WAL

With `--wal`, every keeper runs with a write-ahead log, so clients can make durable writes (see
[Keeper WAL and Durable Writes](../architecture/keeper-wal.md)). The WAL is set for the whole
deployment: in a recording group where some keepers have it off, durable writes to those keepers
fail with `CL_ERR_DURABILITY_UNAVAILABLE`.

### Where the WAL goes

`--start` runs `wal_dir_probe.sh` on each keeper host over ssh and prints the directory it chose.
The base directory is, in this order:

1. one that already holds WAL segments of this install, wherever it ranks, so a restarted keeper or
   a later job on the same node finds what an earlier keeper left;
2. the first existing, writable `<mount point>/$USER` on a local drive other than the boot disk,
   NVMe first, then SATA SSD, then HDD, whose free space minus the reserve is at least `max_bytes`
   (1 GiB). Only a mount with a per-user directory is considered, which keeps discovery off disks
   the site did not hand to users;
3. `${TMPDIR:-/tmp}` on that host, with a warning.

`<hash of the work dir>/<recording group>` is appended to the base, so every deploy from one install
finds the same directory and another install never sees it.

`--wal-dir A:B:C` replaces the discovery with that list. An entry is used if it exists, is a
writable directory and has `max_bytes` free above the reserve; there is no boot-disk check or media
ranking. A leftover WAL of this install in a list entry is chosen before any entry is checked, as
in step 1. The search covers only the list's entries, not `${TMPDIR:-/tmp}`: a leftover there is
named in a warning when an entry is usable, and the keeper does not use it. When no entry is usable
on a keeper host, the deploy prints each entry's reason with the host name and stops before any
daemon is launched. If `${TMPDIR:-/tmp}/<hash>/<group>` holds WAL segments of this install, it
prints one more error line naming that directory, so you can move the segments into an entry's
`<hash>/<group>` or remove them. An empty entry (`/a:`, `:/a`, `/a::/b`) is refused with an error. A
single `--wal-dir` path is used as given, with a warning if it is on the boot disk.

The deploy, and each keeper at startup, warn when the WAL directory is on the boot disk or on NFS,
Lustre, GPFS, BeeGFS, CIFS, Ceph, FUSE, overlay or tmpfs.

### What a durable write survives

| WAL on | Survives |
|---|---|
| A local disk | A keeper crash and a node reboot. The events are replayed when a keeper starts again on that node with the same install. |
| tmpfs | A keeper crash only; a reboot or the end of the job loses them. |
| NFS | Also the loss of the node, if someone restarts a keeper on that directory, but only as durable as the server's export (`sync`) and its storage cache. `flock` there depends on the server's lock service. |

No WAL survives losing its disk.

NFS is a fallback, not the recommendation: every sync is a round trip to the server, and the
archive usually shares the same server. Prefer a local NVMe or SATA SSD.

Trim SSDs that hold a WAL, with `fstrim` on a timer or a `discard` mount: the WAL creates and
deletes 64 MiB segments all the time, and an untrimmed, nearly full drive syncs several times
slower.

### Stopping, restarting and cleaning

- Run `--stop` before the job or allocation ends. Each keeper waits for ChronoGrapher to confirm its
  chunks, then writes an `exit-summary` in its WAL directory. `--stop` prints a table with each
  keeper's state: `clean`, `pending` (durable events the grapher has not confirmed), `missing` (no
  `exit-summary`: the keeper crashed or was killed), `empty` (no WAL directory, or an empty one, so
  no `exit-summary` was written) or `unreachable` (the host did not answer). It exits nonzero
  unless all are clean.
- Events left in a WAL are replayed only when a keeper of the same install starts again on that
  node, possibly days later in another job. If `--stop` reports `pending` or `missing`, start the
  deployment again from the same install on the same nodes, so those keepers replay their WALs and
  the graphers confirm the events, then stop again. Start it with the same options as before,
  including `--wal` or `--wal-dir`, and with the hosts in the same order. Without `--wal` the
  keepers run with no WAL and the leftover events stay where they are. Another host order can put a
  host in another recording group, and the probe then looks for `<hash>/<group>` of the new group
  and does not find the old WAL.
- A build with another WAL format version needs the WAL directories removed before it is installed.
  A clean stop still leaves the active segment and a spare in each, and a keeper refuses to start on
  segments of a format it cannot read. Run `--stop`, then either `--clean` or, after a clean
  `--stop`, remove each keeper's `<base>/<hash>/<group>` directory by hand. `--clean` removes much
  more than the WAL: the HDF5 archive in the output directory, the generated confs, the per-group
  hosts files and the logs. It refuses, removing nothing, while a keeper's WAL state is `pending`,
  `missing` or `unreachable`; `--force` removes the WAL directories anyway and discards those
  events. Do either before the install: `--clean` finds the WAL directories through the generated
  keeper confs.
- After a crash of one keeper while the visor and graphers still run, restart it with
  `--restart-keeper <host>`. It replays its WAL, registers again, and the visor gives it the stories
  clients still hold. After `--stop` it does not apply: start the whole deployment again as described
  above.

### On Ares

The boot SSD is small, is wiped between jobs, and must not hold a WAL. Discovery finds
`/mnt/nvme/$USER`, `/mnt/ssd/$USER` and `/mnt/hdd/$USER`, in that order, so create the per-user
directory on the drives you want used. An NVMe that is nearly full, with less than `max_bytes` free
above the reserve (the larger of 256 MiB and 10 % of the drive), is skipped, and the WAL goes on the
SATA SSD instead. The NVMe drives there are shared, mostly full and mounted without `discard`: on
one, a small append and its sync took 2.3 ms when the drive was quiet and about 5.4 ms after 10 GB
of writes. A sync into a pre-zeroed segment, as the WAL does, took 0.66 ms on it when quiet; it was
not measured after the writes. If no drive
qualifies, the WAL goes to `/tmp`, on the boot disk, with a warning; pass
`--wal-dir /mnt/nvme/$USER:/mnt/ssd/$USER` to fail the deploy instead. A leftover WAL of this
install in `/tmp` is chosen first when no list is given. With a list, it is not used. If an entry is
usable, the deploy prints a warning naming the leftover and goes ahead; if no entry is usable, the
deploy fails and names that directory, so you can move the segments into an entry's
`<hash>/<group>`.

## Execution Modes

Exactly one mode must be specified per invocation:

| Mode | Description |
|------|-------------|
| `--start` | Start all ChronoLog processes across the cluster |
| `--stop` | Stop all ChronoLog processes gracefully. Keepers stop first and get 4 minutes, since each waits for ChronoGrapher to confirm its chunks written; other processes are force-killed after 5 minutes. With the keeper WAL on, it then prints each keeper's WAL state and exits nonzero unless every one is clean (see [Keeper WAL](#keeper-wal)). |
| `--clean` | Remove generated config files, per-group host files and logs, and the archive in the output directory: the files its manifest names, their story directories if empty, then `%manifest`. Other files in the output directory stay. With the keeper WAL on, it also removes the keeper WAL directories, and refuses, removing nothing, while a keeper's state is `pending`, `missing` or `unreachable`, unless `--force` is given; an `empty` one does not stop it. All processes must be stopped first. |
| `--restart-keeper <host>` | Relaunch the keeper on `<host>` with the configuration `--start` generated for it, so it reuses its ports and its WAL directory and replays what the WAL holds. Give it the same path and host options `--start` was given (`--work-dir`, `--keeper-bin`, `--conf-file`, the host files or `--job-id`). `--wal` is not needed: the generated conf already has the WAL on. It is for a deployment whose visor and graphers still run, not for one that `--stop` stopped. |

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
| `--wal` | off | Turn the keeper WAL on (`WAL_ENABLED=1` in the environment does the same); each keeper host gets a WAL directory chosen as described in [Keeper WAL](#keeper-wal) |
| `--wal-dir <A[:B:...]>` | _(none)_ | Turn the keeper WAL on and try these base directories in order instead of the discovery |
| `--force` | `false` | With `--clean`, also remove keeper WAL directories that hold unconfirmed events |
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

Start with the keeper WAL on, then relaunch one keeper after it crashed:

```bash
./deploy_cluster.sh --start --job-id $SLURM_JOB_ID --wal
./deploy_cluster.sh --restart-keeper node03 --job-id $SLURM_JOB_ID
```

Clean up generated files (run after stopping):

```bash
./deploy_cluster.sh --clean
```
