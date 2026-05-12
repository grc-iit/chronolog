# ChronoLog Distributed Append Throughput

Checkpoint: run append throughput workflow against ChronoLog distributed target.

Status: complete.

## Deployment

- Deployment mode: bare metal SLURM allocation.
- Allocation: `debug`, two nodes, job `13677`.
- Nodes: `ares-comp-03`, `ares-comp-04`.
- Transport: `ofi+sockets`.
- Recording groups: 1.
- Launcher: `.agent/scripts/chronolog_run_append_distributed.sh`.

The wrapper uses a real SLURM allocation so Ares' `pam_slurm_adopt` policy permits SSH fan-out from the installed `deploy_cluster.sh`. Per-role wrapper scripts initialize the same Lmod dependency stack used for the validated ChronoLog build before starting `chrono-visor`, `chrono-keeper`, `chrono-grapher`, and `chrono-player`.

## Benchmark

```text
chrono-bench -c .agent/results/20260512-005631/config/default-chrono-client-conf.json -w -h 1 -t 1 -a 512 -s 1024 -b 2048 -n 1 -p
```

## Result

- Metrics: `.agent/results/20260512-005631/chronolog/metrics.json`.
- Client output: `.agent/results/20260512-005631/chronolog/chrono-bench-append-throughput.log`.
- Service logs: `.agent/results/20260512-005631/chronolog/logs/`.
- Config manifest: `.agent/results/20260512-005631/config/chronolog-config-manifest.env`.
- Stop/cleanup evidence: `.agent/results/20260512-005631/chronolog/logs/deploy-stop.log`.

Parsed metrics:

```json
{
  "system": "chronolog",
  "workflow": "append_throughput",
  "node_count": 2,
  "client_count": 1,
  "message_size_bytes": 1024,
  "operation_count": 1,
  "duration_seconds": 3986.954684273059,
  "throughput_ops_per_sec": 0.000250818,
  "avg_latency_ms": null,
  "p50_latency_ms": null,
  "p95_latency_ms": null,
  "p99_latency_ms": null,
  "success": true,
  "record_event_bandwidth_mb_per_sec": 0.264363
}
```

## Notes

This is a distributed append smoke, not a performance claim. It validates the bare-metal distributed launch, client connectivity, comparable metrics schema, and cleanup path. The next report must still justify the selected ChronoLog/Kafka/Mofka configurations and distinguish this append-smoke coverage from the broader provisional workflow suite.
