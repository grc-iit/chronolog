# Phase 0 Status Report

Status: in progress; Phase 0 is not fully complete.

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
- Kafka fixed-baseline distributed append throughput, append latency, and range retrieval smoke runs completed where supported.
- Mofka fixed-baseline distributed append throughput, append latency, and range retrieval smoke runs completed with the current memory-partition path.
- ChronoLog distributed append throughput, append latency, and range retrieval smoke runs completed.
- Common `metrics.json` schema is used by the distributed smoke results.
- Configuration justification written in `.agent/results/phase0-configuration-justification.md`.

## Distributed Workflow Evidence

| Workflow | ChronoLog | Kafka | Mofka |
|---|---|---|---|
| append throughput | `.agent/results/20260512-010115/chronolog/metrics.json` | `.agent/results/20260512-010210/kafka/metrics.json` | `.agent/results/20260512-010240/mofka/metrics.json` |
| append latency | `.agent/results/20260512-010537/chronolog/metrics.json` | `.agent/results/20260512-010656/kafka/metrics.json` | `.agent/results/20260512-010754/mofka/metrics.json` |
| range retrieval | `.agent/results/20260512-015243/chronolog/metrics.json` | `.agent/results/20260512-013547/kafka/metrics.json` | `.agent/results/20260512-011238/mofka/metrics.json` |

These are smoke results only. The operation counts and durations should not be used for performance claims.

## Not Complete

- `mixed_append_read` still needs distributed validation.
- `scaling_sweep` remains unvalidated beyond the two-node distributed runs.
- The distributed workflows still need repeated trials and larger duration/count targets before performance claims.
- ChronoLog distributed profiling outputs still need to be attached to distributed runs where tool permissions allow.
- Mofka Yokan/Warabi-backed partition configuration still needs to be fixed and validated.
- Mofka bundled benchmark generation remains unavailable in the current `~benchmark` install because the ConfigSpace dependency path is absent.

## Resolved Issue

The earlier ChronoLog distributed range retrieval blocker is resolved. The successful run is `.agent/results/20260512-015243/chronolog/metrics.json`.

The harness fixes were:

- Configure the ChronoLog client query callback service with the allocated client's routable 40G IP instead of `127.0.0.1`.
- Run the ChronoLog range client on an allocated compute node.
- Wait for the archived HDF5 story file before issuing `ReplayStory`.

## External Permission Limits

The only true external limitations currently identified are low-level profiling and observability permissions:

- `perf_event_paranoid=4` prevents usable unprivileged `perf stat` and `perf record` collection.
- eBPF-based tools are blocked by `unprivileged_bpf_disabled=2`, root-owned tracing/debugfs mounts, and missing frontend tools.
- `kernel.yama.ptrace_scope=1` is a hardened Linux ptrace policy. It can affect local Mercury shared-memory paths, but it is not a blocker for distributed network-transport runs.

## Next Step

Continue with `mixed_append_read` and the small scaling sweep across ChronoLog, Kafka, and Mofka, using bare-metal SLURM distributed deployment as the primary target.
