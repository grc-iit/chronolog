# Phase 0 Normalized Distributed Append Sweep

Status: complete for the current two-node append-throughput comparison.

This checkpoint reran the append-throughput smoke with normalized high-level workload parameters:

- deployment mode: bare metal
- node count: 2
- client count: 1
- operation count: 10
- message/event size: 1024 bytes

These runs validate comparable measurement plumbing for append throughput. They are still short smoke runs and should not be interpreted as performance conclusions.

## Results

| System | Result Directory | Throughput ops/s | Avg Latency ms | p50 ms | p95 ms | p99 ms |
|---|---|---:|---:|---:|---:|---:|
| ChronoLog | `.agent/results/20260512-010115/` | 0.00197384 | null | null | null | null |
| Kafka | `.agent/results/20260512-010210/` | 19.53125 | 143.6 | 104.0 | 500.0 | 500.0 |
| Mofka | `.agent/results/20260512-010240/` | 9.033536280043712 | 100.57047029258683 | 100.58719897642732 | 100.67095793783665 | 100.67095793783665 |

## Evidence

ChronoLog:

- Metrics: `.agent/results/20260512-010115/chronolog/metrics.json`
- Config manifest: `.agent/results/20260512-010115/config/chronolog-config-manifest.env`
- Nodes: `.agent/results/20260512-010115/config/chronolog-slurm-nodes.txt`
- Client output: `.agent/results/20260512-010115/chronolog/chrono-bench-append-throughput.log`

Kafka:

- Metrics: `.agent/results/20260512-010210/kafka/metrics.json`
- Config manifest: `.agent/results/20260512-010210/config/kafka-config-manifest.env`
- Nodes: `.agent/results/20260512-010210/config/kafka-slurm-nodes.txt`
- Producer output: `.agent/results/20260512-010210/kafka/producer-perf-append-throughput.log`

Mofka:

- Metrics: `.agent/results/20260512-010240/mofka/metrics.json`
- Config manifest: `.agent/results/20260512-010240/config/mofka-config-manifest.env`
- Nodes: `.agent/results/20260512-010240/config/mofka-slurm-nodes.txt`
- Benchmark output: `.agent/results/20260512-010240/mofka/append-benchmark.stdout.log`

## Validation

The common metrics validator accepted all three metrics files:

```text
valid .agent/results/20260512-010115/chronolog/metrics.json
valid .agent/results/20260512-010210/kafka/metrics.json
valid .agent/results/20260512-010240/mofka/metrics.json
```

## Caveats

- ChronoLog `chrono-bench` reports record-event throughput, but not per-operation latency percentiles. The schema therefore records null latency fields for ChronoLog.
- ChronoLog's derived duration comes from `operation_count / reported_throughput`; the wall-clock job completed quickly, so this value should be treated as the benchmark's reported throughput-derived duration, not scheduler wall time.
- The Mofka run uses a memory partition over `ofi+tcp`; the Yokan/Warabi-backed partition remains a configuration issue to fix.
- Kafka remains a fixed baseline and was not tuned.

## Next

The remaining Phase 0 gap is selected workflow coverage beyond append throughput:

- append latency
- range retrieval if supported across all systems
- mixed append/read if supported across all systems
- scaling sweep where cluster limits allow
