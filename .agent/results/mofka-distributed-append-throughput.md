# Mofka Distributed Append Throughput

Status: complete for two-node bare-metal append smoke.

Validation run:

```text
.agent/results/20260512-004535/
```

Command:

```text
.agent/scripts/mofka_run_append_smoke.sh \
  --deployment-mode bare_metal \
  --node-count 2 \
  --protocol ofi+tcp \
  --slurm-partition debug \
  --slurm-time 00:05:00 \
  --operation-count 10 \
  --message-size-bytes 1024
```

Metrics:

```json
{
  "system": "mofka",
  "workflow": "append_throughput",
  "node_count": 2,
  "client_count": 1,
  "message_size_bytes": 1024,
  "operation_count": 10,
  "duration_seconds": 1.1065630769589916,
  "throughput_ops_per_sec": 9.036990487231478,
  "avg_latency_ms": 100.53738678107038,
  "p50_latency_ms": 100.53733095992357,
  "p95_latency_ms": 100.65574501641095,
  "p99_latency_ms": 100.65574501641095,
  "success": true
}
```

Evidence:

- Metrics: `.agent/results/20260512-004535/mofka/metrics.json`
- Selected nodes: `.agent/results/20260512-004535/config/mofka-slurm-nodes.txt`
- Bedrock query before benchmark: `.agent/results/20260512-004535/mofka/bedrock-query-before-benchmark.json`
- Benchmark stdout/stderr: `.agent/results/20260512-004535/mofka/append-benchmark.stdout.log`, `.agent/results/20260512-004535/mofka/append-benchmark.stderr.log`
- Workload config: `.agent/results/20260512-004535/config/mofka-workload.json`
- Summary: `.agent/results/20260512-004535/summary.md`

Observed topology:

- master node: `ares-comp-03`
- storage node: `ares-comp-04`
- master/storage addresses: `ofi+tcp://172.25.101.3:43141`, `ofi+tcp://172.25.101.4:44017`

Configuration note:

- This run uses a Mofka memory partition. The Yokan/Warabi-backed default partition configuration remains an open issue to fix, not a stop condition.
