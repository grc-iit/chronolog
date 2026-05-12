# Phase 0 Final Report

Status: complete for the Phase 0 measurement-pipeline objective.

## Completed

- Branch validated: work is on `opt/phase0-bootstrap`, derived from `origin/develop`.
- ChronoLog baseline build completed.
- ChronoLog TAU profiling abstraction and semantic regions added.
- ChronoLog TAU local smoke produced TAU profile output.
- ChronoLog gperftools CPU and heap profiling produced local outputs.
- ChronoLog Darshan client-side validation produced a local Darshan log.
- Linux network measurement command evidence captured.
- Distributed SLURM access validated on debug compute nodes.
- RDMA/RoCE-capable network evidence captured: `enp47s0np0`, 40 Gb/s, `mlx5_0`, Ethernet link layer.
- Kafka fixed-baseline distributed append throughput, append latency, and range retrieval smoke runs completed.
- Mofka fixed-baseline distributed append throughput, append latency, and range retrieval smoke runs completed with the current memory-partition path.
- ChronoLog distributed append throughput, append latency, and range retrieval smoke runs completed.
- Available distributed scaling sweep evidence was collected at 2 and 4 nodes.
- `mixed_append_read` was documented as unsupported for the selected comparable suite with current ChronoLog archived-playback semantics.
- Common `metrics.json` schema validation passed for selected distributed workflow evidence.
- Configuration justification written in `.agent/results/phase0-configuration-justification.md`.
- Completion audit written in `.agent/results/phase0-completion-audit.md`.

## Distributed Workflow Evidence

| Workflow | ChronoLog | Kafka | Mofka |
|---|---|---|---|
| append throughput | `.agent/results/20260512-010115/chronolog/metrics.json` | `.agent/results/20260512-010210/kafka/metrics.json` | `.agent/results/20260512-010240/mofka/metrics.json` |
| append latency | `.agent/results/20260512-010537/chronolog/metrics.json` | `.agent/results/20260512-010656/kafka/metrics.json` | `.agent/results/20260512-010754/mofka/metrics.json` |
| range retrieval | `.agent/results/20260512-015243/chronolog/metrics.json` | `.agent/results/20260512-013547/kafka/metrics.json` | `.agent/results/20260512-011238/mofka/metrics.json` |

## Scaling Evidence

| Node Count | ChronoLog | Kafka | Mofka |
|---:|---|---|---|
| 2 | `.agent/results/20260512-010115/chronolog/metrics.json` | `.agent/results/20260512-010210/kafka/metrics.json` | `.agent/results/20260512-010240/mofka/metrics.json` |
| 4 | `.agent/results/20260512-020504/chronolog/metrics.json` | `.agent/results/20260512-020415/kafka/metrics.json` | `.agent/results/20260512-020606/mofka/metrics.json` |

One-node runs remain local/single-node smoke and are not counted as final distributed evidence. Eight nodes were not available in the `debug` partition during validation.

## Permission Limits

The only true external limitations currently identified are low-level profiling and observability permissions:

- `perf_event_paranoid=4` prevents usable unprivileged `perf stat` and `perf record` collection without admin-provided capabilities or a profiling allocation.
- eBPF-based tools are blocked by `unprivileged_bpf_disabled=2`, root-owned tracing/debugfs mounts, and missing frontend tools.
- `kernel.yama.ptrace_scope=1` is a hardened Linux ptrace policy. It can affect local Mercury shared-memory paths, but it is not a blocker for distributed network-transport runs.

## Follow-Ups Before Performance Claims

- Increase operation counts, durations, warmup, and repetitions.
- Keep all systems on normalized workload parameters for any performance comparison.
- Fix and validate Mofka Yokan/Warabi-backed storage configuration before storage-backend comparisons.
- Collect full `perf` and eBPF-based observability after cluster/admin permission changes.
- Treat the current results as measurement-pipeline validation only, not performance conclusions.
