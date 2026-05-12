# Phase 0 Status Report

Status: measurement pipeline partially complete; Phase 0 is not fully complete.

## Completed

- Branch validated: work is on `opt/phase0-bootstrap`, derived from `origin/develop`.
- ChronoLog baseline build completed.
- ChronoLog TAU profiling abstraction and semantic regions added.
- ChronoLog TAU local smoke produced TAU profile output.
- ChronoLog gperftools CPU and heap profiling produced local outputs.
- ChronoLog Darshan client-side validation produced a local Darshan log.
- Linux network measurement command evidence captured.
- Distributed SLURM access validated on two debug nodes.
- RDMA/RoCE-capable network evidence captured: `enp47s0np0`, 40 Gb/s, `mlx5_0`, Ethernet link layer.
- Kafka fixed-baseline distributed append smoke completed.
- Mofka fixed-baseline distributed append smoke completed.
- ChronoLog distributed append smoke completed.
- Common `metrics.json` schema is used by the distributed append smoke results.
- Configuration justification written in `.agent/results/phase0-configuration-justification.md`.

## Distributed Append Evidence

| System | Result | Nodes | Operations | Message Size | Metrics |
|---|---:|---:|---:|---:|---|
| ChronoLog | success | 2 | 1 | 1024 bytes | `.agent/results/20260512-005631/chronolog/metrics.json` |
| Kafka | success | 2 | 100 | 1024 bytes | `.agent/results/20260512-004857/kafka/metrics.json` |
| Mofka | success | 2 | 10 | 1024 bytes | `.agent/results/20260512-004535/mofka/metrics.json` |

These are smoke results only. The operation counts are not normalized and should not be used for performance claims.

## Not Complete

- The broader provisional suite is not complete:
  - `append_latency`
  - `range_retrieval`
  - `mixed_append_read`
  - `scaling_sweep`
- The distributed append workflow still needs a normalized count or duration target, repeated trials, and consistent client placement.
- ChronoLog distributed profiling outputs still need to be attached to distributed runs where tool permissions allow.
- Mofka Yokan/Warabi-backed partition configuration still needs to be fixed and validated.
- Mofka bundled benchmark generation remains unavailable in the current `~benchmark` install because the ConfigSpace dependency path is absent.

## External Permission Limits

The only true external limitations currently identified are low-level profiling and observability permissions:

- `perf_event_paranoid=4` prevents usable unprivileged `perf stat` and `perf record` collection.
- eBPF-based tools are blocked by `unprivileged_bpf_disabled=2`, root-owned tracing/debugfs mounts, and missing frontend tools.
- `kernel.yama.ptrace_scope=1` is a hardened Linux ptrace policy. It can affect local Mercury shared-memory paths, but it is not a blocker for distributed network-transport runs.

## Next Step

Run a normalized distributed append sweep across ChronoLog, Kafka, and Mofka with:

- same operation count or duration target
- fixed message size
- fixed client count and placement
- repeated trials
- explicit transport/interface selection
- per-run node, memory, network, storage, and process-topology manifests
