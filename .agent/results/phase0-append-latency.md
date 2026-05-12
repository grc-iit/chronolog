# Phase 0 Distributed Append Latency

Status: complete for the current two-node append-latency workflow.

This checkpoint validates the `append_latency` workflow across ChronoLog, Kafka, and Mofka using common high-level parameters:

- deployment mode: bare metal
- node count: 2
- client count: 1
- operation count: 10
- message/event size: 1024 bytes

## Results

| System | Result Directory | Avg ms | p50 ms | p95 ms | p99 ms | Throughput ops/s |
|---|---|---:|---:|---:|---:|---:|
| ChronoLog | `.agent/results/20260512-010537/` | 1.32669098675251 | 0.36741001531481743 | 8.178920950740576 | 8.178920950740576 | 458.96777564313095 |
| Kafka | `.agent/results/20260512-010656/` | 150.8 | 116.0 | 466.0 | 466.0 | 21.052632 |
| Mofka | `.agent/results/20260512-010754/` | 100.46268810983747 | 100.50473397132009 | 100.63512297347188 | 100.63512297347188 | 9.041991481548225 |

## Evidence

ChronoLog:

- Metrics: `.agent/results/20260512-010537/chronolog/metrics.json`
- Config manifest: `.agent/results/20260512-010537/config/chronolog-config-manifest.env`
- Nodes: `.agent/results/20260512-010537/config/chronolog-slurm-nodes.txt`
- Latency harness output: `.agent/results/20260512-010537/chronolog/chronolog-append-latency.log`

Kafka:

- Metrics: `.agent/results/20260512-010656/kafka/metrics.json`
- Config manifest: `.agent/results/20260512-010656/config/kafka-config-manifest.env`
- Nodes: `.agent/results/20260512-010656/config/kafka-slurm-nodes.txt`
- Producer perf output: `.agent/results/20260512-010656/kafka/producer-perf-append-throughput.log`

Mofka:

- Metrics: `.agent/results/20260512-010754/mofka/metrics.json`
- Config manifest: `.agent/results/20260512-010754/config/mofka-config-manifest.env`
- Nodes: `.agent/results/20260512-010754/config/mofka-slurm-nodes.txt`
- Workload config: `.agent/results/20260512-010754/config/mofka-workload.json`

## Validation

The common metrics validator accepted all three metrics files:

```text
valid .agent/results/20260512-010537/chronolog/metrics.json
valid .agent/results/20260512-010656/kafka/metrics.json
valid .agent/results/20260512-010754/mofka/metrics.json
```

## Implementation Notes

- ChronoLog uses `.agent/scripts/chronolog_append_latency.py`, which calls the installed Python client binding and measures `StoryHandle.log_event` latency after connect/create/acquire setup.
- Kafka uses the existing fixed-baseline producer performance tool; the workflow label is now configurable as `append_latency`.
- Mofka uses the existing Python append benchmark, which already measures per-operation append latency; the workflow label is now configurable as `append_latency`.

These are still smoke-scale runs and are not final performance claims.
