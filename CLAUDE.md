# CLAUDE.md — ChronoLog dev notes

Project-specific instructions for Claude. The single source of truth for the
build/install/deploy/bench loop is the deploy script set under
`tools/deploy/`. Always go through these scripts; do not invoke
`cmake`/`make` directly.

## System overview

ChronoLog is a distributed log storage ecosystem that uses physical time as
the ordering mechanism - eliminating centralized sequencers and enabling
auto-tiered storage across multiple layers. Built to capture the velocity
and variety of modern activity data: from scientific instruments producing
terabytes per second to AI agent audit trails.

## Deployment modern

ChronoLog is expected to be deployed in a distributed environment, either on
a cluster with multiple physical nodes or a swarm of containers. Each service
component (i.e., ChronoVisor, ChronoKeeper, ChronoGrapher and ChronoPlayer) is
preferred to run on a dedicated node/container. Co-located services are possible
too. Client apps may co-located with ChronoKeeper nodes. All nodes (services
and client apps) share a global file system. It could be a NFS (v3 or v4), or a
Parallel File System (PFS) such as Lustre. It will be used to persist global
visible files for the system. Node/container-local storage can be used to store
files that components do not need. In a cluster, inter-node communication is
preferred to go through high-speed network (e.g., 40+Gbps Ethernet).

## End-to-end test loop

Run from repo root: `/home/kfeng/CLionProjects/ChronoLog_dev/latest/ChronoLog`

### 1. Build (Debug)

```bash
tools/deploy/local_single_user_deploy.sh -b -t Debug
```

- Build tree: `~/chronolog-build/Debug/`
- For Release/RelWithDebInfo: `-t Release` / `-t RelWithDebInfo`
- Build types are not interchangeable — `NDEBUG` is defined for Release/RelWithDebInfo and `LOG_DEBUG` plus any `#ifndef NDEBUG` blocks are compiled out.

### 2. Install

```bash
tools/deploy/local_single_user_deploy.sh -i -t Debug
```

- Install tree: `~/chronolog-install/chronolog/`
- Binaries: `~/chronolog-install/chronolog/bin/{chrono-visor,chrono-keeper,chrono-grapher,chrono-player}`
- Config templates: `~/chronolog-install/chronolog/conf/`

### 3. Deploy daemons

```bash
~/chronolog-install/chronolog/tools/deploy/deploy_local.sh -d -w ~/chronolog-install/chronolog -k num_kp -r num_rg
```

This brings up visor → num_rg graphers → numn_rg players → num_kp keepers, each with a generated
per-component config file under `conf/`.

**Before deploying, always check for leftover processes** — the script does
not detect prior runs and a stale daemon will hold ports (4444, 2525, 6666,
3333, 7878, 8888, etc.) and the new launches will segfault on startup. Symptom in
the deploy output:

```
[error] Could not initialize hg_class with protocol ofi+sockets://127.0.0.1:<port>
... line 333: <pid> Segmentation fault (core dumped) ...
```

To clean up before redeploying:

```bash
pkill -9 -f "chrono-(visor|keeper|grapher|player) --config"
sleep 2
```

The `-s` (stop) action of `deploy_local.sh` only kills the most recent set, so
if multiple deploys have accumulated, prefer the `pkill` above.

### 4. Run the chrono-bench MPI write test

```bash
.spack-env/view/bin/mpiexec -n 8 \
  ~/chronolog-install/chronolog/tools/benchmark/chrono-bench \
  -c ~/chronolog-install/chronolog/conf/default-chrono-client-conf.json \
  -w -n 10 -t 1 -h 1 -a 4096 -s 4096 -b 4096 -p
```

- `-n 10`  events per proc
- `-t 1`   stories per proc
- `-h 1`   chronicles per proc
- `-a/-s/-b 4096` min/avg/max event payload bytes
- `-w`     write mode
- `-p`     print stats

Output is the throughput summary (Connect/Create/Acquire/Release/Destroy/Disconnect rates plus E2E and record-event bandwidths).

### 5. Run the chrono-bench tail-read latency test

```bash
.spack-env/view/bin/mpiexec -n 8 \
  ~/chronolog-install/chronolog/tools/benchmark/chrono-bench \
  -c ~/chronolog-install/chronolog/conf/default-chrono-client-conf.json \
  -l -n 64 -t 1 -h 1 -a 4096 -s 4096 -b 4096 -e 500 -m 120
```

- `-l`     latency mode (write + poll `playback()` until each event is visible)
- `-n 64`  events per proc
- `-t 1`   stories per proc
- `-h 1`   chronicles per proc
- `-a/-s/-b 4096` min/avg/max event payload bytes
- `-e 500` poll interval between `playback()` calls (ms)
- `-m 120` max wait for an event to become visible (sec; must cover the chunk sealing window)

Source: `tools/benchmark/perf_bench.cpp` (installed as `chrono-bench`). Output is the “Tail-read latency / cost results” block (send→visible, `playback()`, `log_event()` distributions). Success: non-zero events logged and `never seen (unsealed/evicted): 0`.

### 6. Run the archive read test

Write with the distributed telemetry example (unlike `chrono-bench -w`, it leaves chronicles/stories in place), wait for seal/archive plus the player acceptance window (~300s by default), then replay with reader-to-csv:

```bash
# Write path — chronicle = hostname; stories = cpu_usage / memory_usage / network_usage
.spack-env/view/bin/mpiexec -n 2 \
  ~/chronolog-install/chronolog/examples/chrono-client-example-distributed-telemetry-writer \
  -c ~/chronolog-install/chronolog/conf/default-chrono-client-conf.json \
  -d 30 -i 5

# Archive read — ReplayStory over the HDF5 archive into a CSV
# (may need to retry until events have sealed/archived and aged past the acceptance window)
~/chronolog-install/chronolog/examples/chrono-client-example-reader-to-csv \
  -c ~/chronolog-install/chronolog/conf/default-chrono-client-conf.json \
  -r "$(hostname)" -s cpu_usage -i 3600 -f /tmp/chronolog_replay.csv
```

- `-d 30` / `-i 5`  writer duration / sample interval (seconds)
- `-r` / `-s`  chronicle / story to replay (`$(hostname)` / `cpu_usage` match the writer)
- `-i 3600`  replay the last N seconds
- `-f`  output CSV path

Sources: `client/cpp/examples/client_lib_distributed_telemetry_writer_example.cpp`, `client/cpp/examples/client_lib_reader_to_csv_example.cpp`. Success is a non-empty CSV (`wc -l /tmp/chronolog_replay.csv` > 0).

### 7. Stop daemons

```bash
~/chronolog-install/chronolog/tools/deploy/deploy_local.sh -s -w ~/chronolog-install/chronolog
```

If that doesn't fully clean up, fall back to the `pkill -9` from step 3.

## Full distributed test (mirrors the CI pipeline)

The loop above is a **native, single-host** deployment. The PR check that actually
gates a merge is `.github/workflows/distributed-pipeline.yml`, which runs a
**7-container** deployment (1 visor, 2 keepers, 2 graphers, 2 players) and exercises
things the native loop cannot: cross-host RPC, `hosts_*` files, `parallel-ssh`
process verification, and MPI client ranks spread over several containers.

To run that same pipeline locally against the working tree, before pushing:

```bash
docker/local_distributed_ci.sh                 # same topology as CI (7 containers)
docker/local_distributed_ci.sh -k 1 -g 1 -p 1  # smaller/faster (4 containers)
docker/local_distributed_ci.sh --keep          # leave containers up to debug
docker/local_distributed_ci.sh --skip-build    # reuse the install in the volume
```

It mirrors the workflow stage for stage — compose generation, user/SSH setup,
build+install inside `c1` (shared to the rest via a volume), hosts files,
deployment via `single_user_deploy.sh`, process-distribution verification, the
performance test, the read-path test (write → tail read → archive read), and the
Python tail read with the writer on `c2` and the reader on `c3`. Same assertions,
so a failure here is a failure there.

It needs a base image containing spack and the build dependencies (the same thing
`chronolog-base.yml` builds); pass one with `-i IMAGE`. Note this is **not** what
`docker/dynamic_deploy.sh` does — that stands a cluster up from a pre-built release
image to try ChronoLog out, and cannot test your changes. The workflow does not use
`dynamic_deploy.sh`, and the two compose generators have drifted apart.

**When to run which:**

| Change | Native loop | Distributed script |
|---|---|---|
| Keeper/client logic, unit-testable | yes | not usually |
| Anything touching RPC endpoints, ports or config plumbing | yes | **yes** |
| Deploy scripts, `hosts_*`, service placement | no | **yes** |
| The CI workflow itself | no | **yes** |

## Where to look when something fails

| Symptom | First place to check |
| --- | --- |
| Daemon segfaults on launch | `~/chronolog-install/chronolog/monitor/<component>-1.launch.log` |
| Daemon runs but does the wrong thing | `~/chronolog-install/chronolog/monitor/<component>-1.log` |
| Config not picked up after edit | the *generated* per-component `conf/chrono-<component>-conf-1.json` (deploy regenerates these from the template each time) |
| Port already in use | leftover daemon — see `pkill` recipe in step 3 |

## Action policy reminders

- Never edit `default_conf.json.in` (source tree) just to tweak a running daemon's behavior — patch the build-tree copy under `~/chronolog-build/<type>/tools/cli/default_conf.json` instead. The source template is staged and committed; the build-tree copy is regenerated on every install.
- Skip `chrono_common/src/deprecated_ConfigurationManager.cpp` when refactoring config — it's not built (no CMake reference).
- Cursor-style build dirs (`cmake-build-debug`, `cmake-build-release` at repo root) are IDE-managed and should be ignored — work through `~/chronolog-build/<type>/` only.

## Before committing — format C/C++ changes

Always run clang-format-18 on changed C/C++ sources using the project style
before staging/committing:

```bash
clang-format-18 -i --style=file:.github/code-style/ChronoLog.clang-format <changed .h/.cpp files>
```

Then verify clean (no remaining diff) before committing, e.g.:

```bash
clang-format-18 --style=file:.github/code-style/ChronoLog.clang-format <file> | diff - <file>
```

Only `.h`/`.cpp` sources — do not run it on `.json`/`.json.in` config files.

# Code Review & Pull Request Guidelines for Claude CLI

**CRITICAL SYSTEM INSTRUCTION:** Do NOT automatically post code reviews, comments, or Pull Requests directly to GitHub (or any other version control platform).

## Required Workflow

Whenever you are asked to perform a code review, create a PR, or submit comments, you MUST follow this exact sequence:

1. **Draft Locally:** Prepare your review comments, PR title, and PR description locally.
2. **Present to User:** Output the drafted content in the terminal/chat so I can review it.
3. **WAIT FOR PERMISSION:** Stop and explicitly ask for my confirmation before taking any external actions (e.g., calling GitHub APIs, pushing branches, or submitting PRs).
4. **Execute Only Upon Approval:** Only proceed with creating the PR or posting the review *after* I have explicitly replied with an approval (e.g., "approved", "go ahead", "post it", "looks good").

## Prohibited Actions (Unless Explicitly Approved)

- Do not leave any trace of Claude such as co-authored-by, Claude session id, etc
- `gh pr create` (or similar API/CLI calls)
- `gh pr review` (or similar API/CLI calls)
- Pushing new branches directly to remote repositories (`git push origin <branch>`).
- Submitting issue comments.

**Context:** Your purpose in this repository is to act as an assistant, not an autonomous agent. Always err on the side of asking for permission before interacting with remote git servers.
