# Kafka Baseline Append Throughput

Status: complete.

Kafka 2.8.0 was installed project-locally under ignored `opt/kafka` because no Kafka module or PATH install was available. No Kafka source code was modified.

Successful run:

- Result directory: `.agent/results/20260511-234455/`
- Workflow: `append_throughput`
- Operations: 1000
- Message size: 1024 bytes
- Throughput: 1264.222503 ops/sec
- Average latency: 183.28 ms
- p99 latency: 276 ms

Evidence:

- `.agent/results/20260511-234455/kafka/producer-perf-append-throughput.log`
- `.agent/results/20260511-234455/kafka/metrics.json`
- `.agent/results/20260511-234455/kafka/kafka-source-status.txt`

Note: this environment reaped detached Kafka child processes after the launcher command exited. The validated run kept the launcher shell alive while the benchmark executed, then stopped Kafka using the recorded pids.
