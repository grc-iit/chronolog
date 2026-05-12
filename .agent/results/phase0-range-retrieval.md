# Phase 0 Range Retrieval Support

Status: partial.

Kafka and Mofka completed distributed range/read retrieval smoke runs. ChronoLog exposed a `ReplayStory` API path, but the distributed range retrieval run is blocked by a repeatable hang followed by a ChronoVisor abort.

## Common Parameters

- deployment mode: bare metal
- node count: 2
- client count: 1
- operation count: 10
- message/event size: 1024 bytes

## Kafka

Status: complete.

- Metrics: `.agent/results/20260512-013547/kafka/metrics.json`
- Config manifest: `.agent/results/20260512-013547/config/kafka-config-manifest.env`
- Consumer output: `.agent/results/20260512-013547/kafka/consumer-perf-range-retrieval.log`

Kafka used `kafka-consumer-perf-test.sh` after producing the test records to the topic.

```json
{
  "system": "kafka",
  "workflow": "range_retrieval",
  "node_count": 2,
  "client_count": 1,
  "message_size_bytes": 1024,
  "operation_count": 10,
  "duration_seconds": 5.359918529238356,
  "throughput_ops_per_sec": 1.8657,
  "avg_latency_ms": null,
  "p50_latency_ms": null,
  "p95_latency_ms": null,
  "p99_latency_ms": null,
  "success": true
}
```

## Mofka

Status: complete.

- Metrics: `.agent/results/20260512-011238/mofka/metrics.json`
- Config manifest: `.agent/results/20260512-011238/config/mofka-config-manifest.env`
- Benchmark output: `.agent/results/20260512-011238/mofka/append-benchmark.stdout.log`

Mofka used `TopicHandle.consumer()` and `Consumer.pull()` after producing the test records.

```json
{
  "system": "mofka",
  "workflow": "range_retrieval",
  "node_count": 2,
  "client_count": 1,
  "message_size_bytes": 1024,
  "operation_count": 10,
  "duration_seconds": 0.0029256989946588874,
  "throughput_ops_per_sec": 3417.9866138846996,
  "avg_latency_ms": 0.025932013522833586,
  "p50_latency_ms": 0.016406062059104443,
  "p95_latency_ms": 0.10212801862508059,
  "p99_latency_ms": 0.10212801862508059,
  "success": true
}
```

## ChronoLog

Status: blocked.

Attempt 1:

- Result directory: `.agent/results/20260512-011348/`
- Outcome: `ReplayStory` did not return before the SLURM allocation expired.
- Cleanup then lost compute-node SSH access because `pam_slurm_adopt` requires an active job.

Attempt 2:

- Result directory: `.agent/results/20260512-012853/`
- Outcome: internal `timeout 300s` expired before `ReplayStory` returned.
- Cleanup completed before allocation expiry.
- ChronoVisor launch log records:

```text
Function returned HG_NOENTRY
terminate called after throwing an instance of 'thallium::margo_exception'
what(): [margo_respond] Function returned HG_NOENTRY
```

Evidence:

- ChronoLog range harness: `.agent/scripts/chronolog_range_retrieval.py`
- ChronoLog failed run stdout/stderr: `.agent/results/20260512-012853/chronolog/stdout.log`, `.agent/results/20260512-012853/chronolog/stderr.log`
- ChronoVisor abort evidence: `.agent/results/20260512-012853/chronolog/logs/chrono-visor-ares-comp-03.launch.log`
- Cleanup evidence: `.agent/results/20260512-012853/chronolog/logs/deploy-stop.log`

## Conclusion

Range retrieval is not complete across all three systems. Kafka and Mofka have distributed read evidence. ChronoLog needs ReplayStory/debugging work before the range retrieval workflow can be counted as complete for Phase 0.
