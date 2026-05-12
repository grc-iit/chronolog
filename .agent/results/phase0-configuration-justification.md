# Phase 0 Configuration Justification

Status: complete for the current distributed append-throughput smoke comparison.

This document justifies the selected configurations used for the first distributed ChronoLog, Kafka, and Mofka append-throughput evidence. It does not claim the broader provisional suite is complete.

## Deployment Policy

The final target is distributed bare-metal deployment through SLURM. Local single-node runs on `ares` are smoke tests only.

Bare metal is preferred because ChronoLog and Mofka are HPC-oriented systems and the later optimization work is expected to depend on RDMA/RoCE-capable network configuration. Containerized deployment remains a fallback if it unblocks mechanics, but it is not the preferred comparison mode.

Current distributed evidence uses the `debug` partition and two nodes:

- `ares-comp-03`
- `ares-comp-04`

Network evidence from `.agent/results/distributed-slurm-network-evidence.md` shows:

- `enp47s0np0` on `172.25.101.x/16`
- 40 Gb/s link speed
- RDMA device `mlx5_0`
- Ethernet link layer, consistent with RoCE-capable hardware exposure

## Common Workload

The only workflow currently validated in distributed mode across all three systems is:

- workflow: `append_throughput`
- message/event size: 1024 bytes
- client count: 1
- deployment mode: bare metal

The operation count differs across early smoke runs because the first goal was end-to-end distributed validation, not performance ranking:

- ChronoLog: 1 event
- Kafka: 100 messages
- Mofka: 10 records

The final comparison sweep should normalize operation count, warmup, duration target, client placement, and repetition count before using the results for performance claims.

## ChronoLog

Evidence:

- Metrics: `.agent/results/20260512-005631/chronolog/metrics.json`
- Report: `.agent/results/chronolog-distributed-append-throughput.md`
- Config manifest: `.agent/results/20260512-005631/config/chronolog-config-manifest.env`

Selected configuration:

- launcher: installed `deploy_cluster.sh`
- allocation: active SLURM allocation, then SSH fan-out permitted by Ares `pam_slurm_adopt`
- topology: `chrono-visor`, `chrono-keeper`, `chrono-grapher`, `chrono-player`
- recording groups: 1
- transport: `ofi+sockets`
- client: `chrono-bench`
- dependency stack: same Lmod module set used for the validated ChronoLog build

Why this is acceptable for Phase 0:

- It validates distributed service launch, client connectivity, logging, cleanup, and common `metrics.json` generation.
- It uses the existing ChronoLog deployment helper rather than inventing a separate service topology.
- It records the exact deployment, node list, transport, benchmark command, and logs.

What still needs improvement before performance claims:

- Replace the one-event smoke with a normalized duration/count sweep.
- Evaluate `ofi+tcp` or RDMA/RoCE-capable libfabric provider choices explicitly.
- Capture CPU binding, service thread/pool settings, filesystem target, and node memory in the per-run manifest.
- Keep TAU/perf/gperftools/Darshan/eBPF-based observability attached to ChronoLog runs where permissions allow.

## Kafka

Evidence:

- Metrics: `.agent/results/20260512-004857/kafka/metrics.json`
- Report: `.agent/results/kafka-distributed-append-throughput.md`
- Config manifest: `.agent/results/20260512-004857/config/kafka-config-manifest.env`

Selected configuration:

- ZooKeeper node: `ares-comp-03`
- broker node: `ares-comp-04`
- listener: `172.25.101.4:30092`
- topic partitions: 1
- replication factor: 1
- producer acknowledgements: `acks=1`
- JVM heap: recorded in manifest

Why this is acceptable for Phase 0:

- Kafka is a fixed baseline; the pipeline records configuration but does not tune Kafka internals.
- One broker and one partition are sufficient for the first two-node append smoke and keep the baseline simple.
- The run uses Kafka's standard `kafka-producer-perf-test.sh`, so parsing and latency fields are available.

What still needs improvement before performance claims:

- Normalize producer count, record count, test duration, batching/linger/compression defaults, and repetition count.
- Decide whether the final baseline should stay single-broker or include a multi-broker topology for node-scaling comparisons.
- Record disk/log directory filesystem details for each run.

## Mofka

Evidence:

- Metrics: `.agent/results/20260512-004535/mofka/metrics.json`
- Yokan/Warabi-backed append validation: `.agent/results/20260512-091538/mofka/metrics.json`
- Yokan/Warabi-backed range validation: `.agent/results/20260512-093629/mofka/metrics.json`
- Report: `.agent/results/mofka-distributed-append-throughput.md`
- Bedrock query: `.agent/results/20260512-004535/mofka/bedrock-query-before-benchmark.json`

Selected configuration:

- deployment mode: bare metal
- protocol: `ofi+tcp`
- master node: `ares-comp-03`
- storage node: `ares-comp-04`
- partition type for storage validation: default
- metadata/data providers: Mofka default partition path backed by configured Yokan metadata and Warabi data providers
- benchmark client: `.agent/scripts/mofka_append_benchmark.py`

Why this is acceptable for Phase 0:

- It validates distributed Bedrock launch, provider visibility, append path, metrics generation, and cleanup.
- The default partition validation exercises the intended Mofka Yokan/Warabi-backed storage path instead of relying only on memory partitions.
- Mofka remains a fixed baseline; configuration is recorded and launched, not optimized.

What still needs improvement before performance claims:

- Build or expose the `+benchmark` Mofka tooling path if `mofkactl benchmark generate` will be used as a reference workload source.
- Evaluate the RDMA/RoCE-capable provider path after interface/provider selection is agreed.
- Record pool/execution-stream settings, provider layout, storage target type/path, persistence flags, and partition count in every run manifest.

## External Permission Limits

Only the low-level profiling/observability permissions are true external limitations at this stage:

- `perf_event_paranoid=4` blocks usable unprivileged `perf stat` and `perf record` output without capabilities such as `CAP_PERFMON`, `CAP_SYS_PTRACE`, or `CAP_SYS_ADMIN`.
- `/proc/sys/kernel/unprivileged_bpf_disabled=2`, `kptr_restrict=1`, and root-owned tracing/debugfs mounts block eBPF-based tools in the current environment.
- `kernel.yama.ptrace_scope=1` restricts ptrace-style same-user process access. It affects local shared-memory style probing/transport behavior, but it is not a distributed deployment blocker when using network transports such as `ofi+tcp` or `ofi+sockets`.

The admin-facing ask is therefore specific: enable an approved profiling allocation or capabilities for `perf` and eBPF-based tools, and separately clarify whether local shared-memory Mercury paths are expected to be allowed on Ares.

## Current Conclusion

The current configuration is good enough to demonstrate the distributed measurement pipeline for append-throughput smoke tests. It is not yet good enough for final performance conclusions.

The next defensible step is a normalized distributed append sweep across ChronoLog, Kafka, and Mofka using the same operation count or duration target, fixed client placement, repeated trials, and explicit network/provider configuration.
