# Phase 0 Scaling Sweep

Status: complete for available distributed bare-metal smoke node counts.

The provisional suite requested 1, 2, 4, and 8 nodes where cluster limits allow. For the Phase 0 distributed target:

- 1 node is excluded from final evidence because the user clarified that local/single-node smoke is not the target.
- 2-node distributed append throughput and append latency evidence already exists for ChronoLog, Kafka, and Mofka.
- 4-node distributed append throughput smoke evidence was collected on `ares-comp-[03-06]`.
- 8 nodes were not available in the `debug` partition during validation; `sinfo` showed five idle nodes and one invalid node.

These runs validate scaling-harness mechanics and result collection only. They are not performance claims.

## 2-Node Evidence

| Workflow | ChronoLog | Kafka | Mofka |
|---|---|---|---|
| append throughput | `.agent/results/20260512-010115/chronolog/metrics.json` | `.agent/results/20260512-010210/kafka/metrics.json` | `.agent/results/20260512-010240/mofka/metrics.json` |
| append latency | `.agent/results/20260512-010537/chronolog/metrics.json` | `.agent/results/20260512-010656/kafka/metrics.json` | `.agent/results/20260512-010754/mofka/metrics.json` |

## 4-Node Evidence

| System | Result | Metrics | Notes |
|---|---|---|---|
| ChronoLog | success | `.agent/results/20260512-020504/chronolog/metrics.json` | One ChronoVisor, four ChronoKeepers, one ChronoGrapher, one ChronoPlayer; bare-metal SLURM allocation. |
| Kafka | success | `.agent/results/20260512-020415/kafka/metrics.json` | One ZooKeeper node and three broker nodes after multi-broker launcher update. |
| Mofka | success | `.agent/results/20260512-020606/mofka/metrics.json` | One Bedrock master and three storage Bedrock processes with memory partition path. |

## 4-Node Metrics

ChronoLog:

```json
{
  "system": "chronolog",
  "workflow": "append_throughput",
  "node_count": 4,
  "client_count": 1,
  "message_size_bytes": 1024,
  "operation_count": 100,
  "duration_seconds": 29263.724686878144,
  "throughput_ops_per_sec": 0.0034172,
  "avg_latency_ms": null,
  "p50_latency_ms": null,
  "p95_latency_ms": null,
  "p99_latency_ms": null,
  "success": true,
  "record_event_bandwidth_mb_per_sec": 3.44068
}
```

Kafka:

```json
{
  "system": "kafka",
  "workflow": "append_throughput",
  "node_count": 4,
  "client_count": 1,
  "message_size_bytes": 1024,
  "operation_count": 100,
  "duration_seconds": 0.52200000061596,
  "throughput_ops_per_sec": 191.570881,
  "avg_latency_ms": 129.38,
  "p50_latency_ms": 127.0,
  "p95_latency_ms": 138.0,
  "p99_latency_ms": 485.0,
  "success": true
}
```

Mofka:

```json
{
  "system": "mofka",
  "workflow": "append_throughput",
  "node_count": 4,
  "client_count": 1,
  "message_size_bytes": 1024,
  "operation_count": 100,
  "duration_seconds": 10.16546491207555,
  "throughput_ops_per_sec": 9.837228386987993,
  "avg_latency_ms": 100.55693796253763,
  "p50_latency_ms": 100.55721399839967,
  "p95_latency_ms": 100.65883002243936,
  "p99_latency_ms": 100.71874503046274,
  "success": true
}
```

## Validation

The common metrics validator passed for:

- `.agent/results/20260512-020415/kafka/metrics.json`
- `.agent/results/20260512-020504/chronolog/metrics.json`
- `.agent/results/20260512-020606/mofka/metrics.json`

The Kafka fixed-baseline launcher now maps a 4-node run to one ZooKeeper node and three broker nodes, instead of reporting a 4-node allocation while running a single broker.
