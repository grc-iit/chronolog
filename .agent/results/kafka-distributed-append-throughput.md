# Kafka Distributed Append Throughput

Status: complete for two-node bare-metal append smoke.

Validation run:

```text
.agent/results/20260512-004857/
```

Command:

```text
.agent/scripts/kafka_run_append_distributed.sh \
  --partition debug \
  --node-count 2 \
  --slurm-time 00:05:00 \
  --operation-count 100 \
  --message-size-bytes 1024 \
  --zookeeper-port 23181 \
  --broker-port 30092
```

Metrics:

```json
{
  "system": "kafka",
  "workflow": "append_throughput",
  "node_count": 2,
  "client_count": 1,
  "message_size_bytes": 1024,
  "operation_count": 100,
  "duration_seconds": 0.53600000051456,
  "throughput_ops_per_sec": 186.567164,
  "avg_latency_ms": 128.9,
  "p50_latency_ms": 124.0,
  "p95_latency_ms": 137.0,
  "p99_latency_ms": 504.0,
  "success": true
}
```

Evidence:

- Metrics: `.agent/results/20260512-004857/kafka/metrics.json`
- Selected nodes: `.agent/results/20260512-004857/config/kafka-slurm-nodes.txt`
- Config manifest: `.agent/results/20260512-004857/config/kafka-config-manifest.env`
- Producer perf output: `.agent/results/20260512-004857/kafka/producer-perf-append-throughput.log`
- Service logs: `.agent/results/20260512-004857/kafka/logs/`
- Summary: `.agent/results/20260512-004857/summary.md`

Observed topology:

- ZooKeeper node: `ares-comp-03`
- Broker node: `ares-comp-04`
- Broker advertised listener: `172.25.101.4:30092`

Kafka remains a fixed baseline. The script records configuration but does not tune Kafka internals.
