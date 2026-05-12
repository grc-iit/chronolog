# Mofka Append Smoke

- system: Mofka
- workflow: append_latency
- deployment_mode: bare_metal
- protocol: ofi+tcp
- partition_type: memory
- node_count: 2
- operation_count: 10
- message_size_bytes: 1024
- metrics: mofka/metrics.json

This smoke validates the Mofka append path and metrics plumbing. A bare_metal run contributes distributed evidence; a local_smoke run does not satisfy the final distributed benchmark requirement.
