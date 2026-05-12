# ChronoLog Distributed Append Smoke

- system: ChronoLog
- workflow: append_throughput
- deployment_mode: bare_metal
- transport: ofi+sockets
- node_count: 2
- record_groups: 1
- operation_count: 1
- message_size_bytes: 1024
- metrics: chronolog/metrics.json
