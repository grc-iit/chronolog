# Phase 0 Range Retrieval Support

Status: complete for the two-node distributed smoke target.

Kafka, Mofka, and ChronoLog completed distributed range/read retrieval smoke runs. The ChronoLog failures seen in earlier attempts were harness/configuration issues, not an external cluster blocker.

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

Status: complete.

- Metrics: `.agent/results/20260512-015243/chronolog/metrics.json`
- Config manifest: `.agent/results/20260512-015243/config/chronolog-config-manifest.env`
- Client output: `.agent/results/20260512-015243/chronolog/chronolog-range-retrieval.log`
- Archive evidence: `.agent/results/20260512-015243/chronolog/output/phase0_range_chronicle_1778568791663.phase0_range_story_1778568791663.1778568780.vlen.h5`

ChronoLog used the Python client `ReplayStory` path after writing and waiting for the archived story file to exist. The distributed harness now runs the range client on an allocated compute node and configures the `ClientQueryService` callback IP to that node's 40G address.

```json
{
  "system": "chronolog",
  "workflow": "range_retrieval",
  "node_count": 2,
  "client_count": 1,
  "message_size_bytes": 1024,
  "operation_count": 10,
  "duration_seconds": 2.00279733305797,
  "throughput_ops_per_sec": 4.993016435033647,
  "avg_latency_ms": 2002.7973330579698,
  "p50_latency_ms": 2002.7973330579698,
  "p95_latency_ms": 2002.7973330579698,
  "p99_latency_ms": 2002.7973330579698,
  "success": true,
  "retrieved_event_count": 10
}
```

## Resolved ChronoLog Harness Issue

Earlier failed attempts:

- `.agent/results/20260512-011348/`: `ReplayStory` did not return before the SLURM allocation expired.
- `.agent/results/20260512-012853/`: internal `timeout 300s` expired before `ReplayStory` returned; ChronoVisor aborted with `HG_NOENTRY`.
- `.agent/results/20260512-014245/`: callback topology was fixed, but the client queried before the HDF5 archive appeared.

Fixes:

- Set `.chrono_client.ClientQueryService.rpc.service_ip` to the allocated client node's routable 40G IP instead of leaving it on `127.0.0.1`.
- Run the ChronoLog range client from inside the SLURM allocation on the client node.
- Wait for the archived HDF5 story file before issuing `ReplayStory`.

## Conclusion

The `range_retrieval` workflow now has comparable two-node distributed smoke evidence across ChronoLog, Kafka, and Mofka. These are correctness/harness validation runs only; operation counts and durations are still too small for performance claims.
