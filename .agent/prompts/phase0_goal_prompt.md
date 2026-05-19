# Phase 0 Goal Prompt - ChronoLog Measurement Pipeline

You are Codex running in the ChronoLog repository.

Your job is to execute Phase 0 of the ChronoLog optimization framework.

This prompt is intended for Codex `/goal`, not for a blind shell loop.

## Read first

Before doing anything, read:

1. `AGENTS.md`
2. `.agent/TASKS.md`
3. `.agent/VALIDATION.md` if it exists
4. `.agent/results/progress.md`
5. `.agent/state/current.md`
6. existing files under `.agent/results/` if they affect the next task

## Core project scope

ChronoLog is the only target system.

Kafka and Mofka are fixed baselines only.

Do not modify, instrument, tune, or optimize Kafka or Mofka source code.

For Kafka and Mofka, only create or use scripts needed to:

- launch,
- run the selected workload,
- collect throughput/latency,
- stop cleanly.

ChronoLog may be modified during Phase 0 for:

- build fixes,
- profiling abstraction,
- TAU integration,
- semantic instrumentation,
- benchmark integration,
- result collection,
- deployment fixes.

Phase 0 is not a performance-optimization phase. Do not spend Phase 0 chasing speculative ChronoLog speedups. Performance optimization begins after the full measurement pipeline works.

## Branch rule

ChronoLog work must happen on a branch off `develop`.

Before editing, run:

```bash
git branch --show-current
git fetch origin
git merge-base --is-ancestor origin/develop HEAD
```

If not on a suitable branch, create one:

```bash
git checkout develop
git pull origin develop
git checkout -b opt/phase0-bootstrap
```

Do not work directly on `develop`, `main`, or `master`.

## Phase 0 definition

Phase 0 is complete only when:

1. The selected workflows/benchmarks are defined.
2. Required tools are installed or detected without sudo.
3. ChronoLog builds normally.
4. ChronoLog builds with TAU instrumentation.
5. ChronoLog profiling/observability stack produces output:
   - TAU,
   - perf,
   - gperftools,
   - Darshan if applicable,
   - eBPF-based observability if available,
   - Linux network measurement commands.
6. Kafka fixed baseline launches, runs selected workflows, records metrics, and stops.
7. Mofka fixed baseline launches, runs selected workflows, records metrics, and stops.
8. All selected workflows run on ChronoLog, Kafka, and Mofka.
9. Every run produces comparable `metrics.json`.
10. ChronoLog profile artifacts are collected.
11. A final Phase 0 report is written.

## First priority

If the selected workflows/benchmarks are not yet defined, make that the first task.

Look for user-provided benchmark/workflow definitions. If none exist, create a provisional benchmark suite and clearly label it provisional.

The provisional suite should include:

- append/write throughput,
- append/write latency,
- read/range retrieval if supported,
- mixed append/read if supported,
- small scaling sweep: 1, 2, 4, and 8 nodes where cluster limits allow.

Do not pretend benchmark equivalence is obvious. Explicitly document mappings such as:

```text
ChronoLog story/event <-> Kafka topic/message <-> Mofka stream/record
```

or the closest available equivalent.

## Tool installation rule

Assume no sudo.

For every tool, try in this order:

1. already installed command,
2. cluster module,
3. `$HOME/.local` user install,
4. project-local `opt/` install,
5. conda/mamba/spack if available,
6. source build into user-local or project-local prefix.

Do not give up merely because sudo is unavailable.

Document every installed/detected tool in:

```text
.agent/results/toolchain-report.md
```

## ChronoLog profiling stack

Use the following stack for ChronoLog:

```text
TAU
perf
gperftools
Darshan
eBPF-based tools
Linux network measurement commands
```

Use the exact phrase **eBPF-based tools**. Do not list `eBPF / bpftrace / BCC` as three independent categories. eBPF is the kernel mechanism; bpftrace and BCC are frontends/toolkits.

Use the exact phrase **Linux network measurement commands** instead of vague "networking tools."

Required network command checks:

```text
iperf3
ss
nstat
sar -n
ethtool
```

## TAU instrumentation design

Do not scatter raw TAU calls throughout ChronoLog.

Add a small ChronoLog profiling abstraction first.

Preferred shape:

```cpp
CL_PROFILE_REGION("keeper_flush");
CL_PROFILE_COUNTER("append_bytes", bytes);
```

The abstraction must support:

```text
profiling disabled -> no-op
profiling enabled with TAU -> TAU backend
```

Use coarse semantic regions first:

```text
client_append
client_query
rpc_send
rpc_receive
serialization
deserialization
keeper_ingest
keeper_buffer_insert
keeper_flush
storage_write
storage_read
metadata_lookup
story_index_update
grapher_query
range_retrieval
```

## Goal behavior

Work checkpoint-by-checkpoint.

On each checkpoint:

1. Read `.agent/TASKS.md`.
2. Select the first unchecked task.
3. Do only that task unless a blocker requires a prerequisite.
4. Make the smallest useful change.
5. Run validation.
6. Save logs/results under `.agent/results/<timestamp>/`.
7. Update `.agent/results/progress.md`.
8. Update `.agent/state/current.md`.
9. Update `.agent/TASKS.md`.
10. Commit if validation passes.
11. If validation fails, retry with small fixes up to three times.
12. If still blocked, document the blocker and either continue with an independent task or stop if no safe task remains.

## Validation requirements

A task is complete only when:

- command exits with status 0 or blocker is documented,
- logs are saved,
- result files are written when applicable,
- no Kafka/Mofka source changes were made,
- ChronoLog branch rule is satisfied,
- `.agent/results/progress.md` is updated,
- `.agent/state/current.md` is updated,
- `.agent/TASKS.md` is updated,
- successful work is committed.

## Result format

Every run should write to:

```text
.agent/results/YYYYMMDD-HHMMSS/
```

Every system run should produce:

```text
metrics.json
stdout.log
stderr.log
```

ChronoLog should additionally collect:

```text
profiles/
system-metrics/
```

## Commit policy

Commit validated milestones with:

```text
agent: <short description>
```

Do not push to remote.

## Stop policy

Only stop when Phase 0 is complete or when a real blocker prevents safe progress.

If stopping, write:

```text
STOP_RALPH_LOOP
```

and update:

```text
.agent/results/blockers.md
```

## Begin

Now select the first unchecked task in `.agent/TASKS.md` and execute it.
