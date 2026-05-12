# AGENTS.md - ChronoLog Optimization Framework

## Mission

Build the Phase 0 measurement pipeline for the ChronoLog optimization framework.

Phase 0 is not a performance-optimization phase. Phase 0 is complete only when the selected workflows/benchmarks run end-to-end on ChronoLog, Kafka, and Mofka, and ChronoLog produces profiling/observability outputs.

The goal is to make the full measurement machine work before attempting performance improvements.

## Branch rule

All ChronoLog work must happen on a branch created from `develop`.

Required branch pattern:

```text
opt/phase0-*
```

Before making changes, verify:

```bash
git branch --show-current
git merge-base --is-ancestor origin/develop HEAD
```

Do not work directly on:

```text
develop
main
master
```

## Systems

### ChronoLog

ChronoLog is the only target system.

ChronoLog may be:

- built,
- modified,
- instrumented,
- profiled,
- debugged,
- rebuilt,
- rerun.

During Phase 0, ChronoLog source changes should focus on:

- build fixes,
- TAU integration,
- profiling abstraction,
- semantic instrumentation,
- benchmark integration,
- logging/result collection needed for the pipeline.

Phase 0 should not chase speculative performance improvements. Performance optimization begins after the full measurement pipeline works.

### Kafka

Kafka is a fixed baseline only.

Allowed:

- launch Kafka,
- run selected benchmark/workflow,
- collect throughput and latency,
- stop Kafka.

Forbidden:

- do not modify Kafka source code,
- do not instrument Kafka source code,
- do not tune Kafka internals,
- do not optimize Kafka.

### Mofka

Mofka is a fixed baseline only.

Allowed:

- launch Mofka,
- run selected benchmark/workflow,
- collect throughput and latency,
- stop Mofka.

Forbidden:

- do not modify Mofka source code,
- do not instrument Mofka source code,
- do not tune Mofka internals,
- do not optimize Mofka.

## Phase 0 scope

Phase 0 includes:

- no-sudo tool installation or detection,
- SLURM environment discovery,
- ChronoLog baseline build,
- ChronoLog TAU instrumentation,
- ChronoLog profiling stack validation,
- Kafka fixed-baseline launch and benchmark run,
- Mofka fixed-baseline launch and benchmark run,
- shared benchmark/workflow harness,
- common result format,
- final Phase 0 report.

Phase 0 does not include:

- trying to beat Kafka or Mofka,
- algorithmic ChronoLog optimization,
- data-structure replacement for performance,
- storage-layout redesign,
- network-protocol redesign,
- tuning Kafka,
- tuning Mofka.

## Required first question

Before finalizing benchmark scripts, identify or ask for the selected workflows/benchmarks.

If no benchmark list is provided, create a proposed default suite and mark it clearly as provisional.

The default proposed suite should include:

- append/write throughput,
- append/write latency,
- read/range retrieval if supported across systems,
- mixed append/read if supported across systems,
- small scaling sweep: 1, 2, 4, and 8 nodes where cluster limits allow.

## Profiling and observability stack for ChronoLog

### TAU

Use TAU for source-level semantic instrumentation of ChronoLog.

Do not scatter raw TAU calls everywhere. Add a ChronoLog profiling abstraction such as:

```cpp
CL_PROFILE_REGION("keeper_flush");
CL_PROFILE_COUNTER("append_bytes", bytes);
```

The abstraction must compile to no-op when profiling is disabled.

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

### perf

Use `perf` for:

- CPU profiling,
- flamegraphs,
- hardware counters,
- cache behavior,
- branch behavior,
- call-stack hot spots.

### gperftools

Use gperftools for:

- CPU profiling,
- heap profiling,
- allocation profiling,
- malloc/new behavior.

### Darshan

Use Darshan for I/O behavior if applicable to ChronoLog's I/O path.

### eBPF-based observability

Use the phrase **eBPF-based tools**, not `eBPF / bpftrace / BCC` as separate categories.

Meaning:

```text
eBPF:
    Linux kernel mechanism

bpftrace:
    high-level scripting frontend for eBPF

BCC:
    toolkit/library with ready-made eBPF tools
```

Prefer existing bpftrace or BCC tools when available.

Use eBPF-based observability for:

- syscall latency,
- block I/O latency,
- off-CPU time,
- scheduler delay,
- futex/lock contention,
- TCP events.

### Linux network measurement commands

Use the phrase **Linux network measurement commands**, not vague "networking tools."

Required detection targets:

```text
iperf3
ss
nstat
sar -n
ethtool
```

Optional for targeted debugging:

```text
tcpdump
```

Purpose:

- `iperf3`: raw node-to-node bandwidth,
- `ss`: socket state and send/receive queues,
- `nstat`: kernel TCP/IP counters,
- `sar -n`: network throughput over time,
- `ethtool`: NIC speed, driver stats, drops, offload settings,
- `tcpdump`: packet capture only for targeted debugging.

## No-sudo rule

Assume no sudo access.

For dependencies and tools, use this priority order:

1. existing cluster modules,
2. user-local install under `$HOME/.local`,
3. project-local install under the repository or workspace `opt/`,
4. conda/mamba/spack if available without sudo,
5. build from source into a user-local or project-local prefix.

Do not stop just because sudo is unavailable.

Only mark a dependency blocked after documenting attempted module, user-local, and source-build paths.

## Result directory format

Every experiment should write to:

```text
.agent/results/YYYYMMDD-HHMMSS/
```

Expected structure:

```text
.agent/results/YYYYMMDD-HHMMSS/
|-- config/
|-- chronolog/
|   |-- metrics.json
|   |-- stdout.log
|   |-- stderr.log
|   |-- logs/
|   `-- profiles/
|-- kafka/
|   |-- metrics.json
|   |-- stdout.log
|   `-- stderr.log
|-- mofka/
|   |-- metrics.json
|   |-- stdout.log
|   `-- stderr.log
`-- summary.md
```

## Metrics schema

Every system run should produce a comparable `metrics.json` containing at least:

```json
{
  "system": "chronolog|kafka|mofka",
  "workflow": "name",
  "node_count": 0,
  "client_count": 0,
  "message_size_bytes": 0,
  "operation_count": 0,
  "duration_seconds": 0,
  "throughput_ops_per_sec": 0,
  "avg_latency_ms": null,
  "p50_latency_ms": null,
  "p95_latency_ms": null,
  "p99_latency_ms": null,
  "success": true
}
```

## Progress visibility rule

After every checkpoint, update:

```text
.agent/results/progress.md
```

Use this format:

```markdown
| Time | Checkpoint | Status | Evidence | Next |
|---|---|---|---|---|
```

Also update:

```text
.agent/state/current.md
```

with:

- current task,
- commands running,
- last successful validation,
- current blocker if any,
- next intended step.

## Validation policy

Every task must end with validation.

A task is complete only if:

- the relevant command exits successfully,
- logs are saved,
- results are written under `.agent/results/`,
- task status is updated in `.agent/TASKS.md`,
- successful milestones are committed.

## Commit policy

Commit each validated milestone.

Commit message format:

```text
agent: <short milestone description>
```

Do not commit broken builds unless the commit is explicitly documenting a blocker and no code path is made worse.

## Stop conditions

Stop the goal only when:

- Phase 0 is complete,
- the same blocker fails repeatedly after documented attempts,
- required cluster access is unavailable,
- continuing would require sudo,
- continuing would risk destructive changes,
- continuing would exceed configured cluster limits.

When stopping due to a blocker, write:

```text
STOP_RALPH_LOOP
```

and explain the blocker in:

```text
.agent/results/blockers.md
```
