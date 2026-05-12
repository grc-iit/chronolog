# Mofka Append Smoke

- system: Mofka
- workflow: append_throughput
- deployment_mode: local_smoke
- protocol: ofi+tcp
- partition_type: memory
- operation_count: 50
- message_size_bytes: 1024
- metrics: mofka/metrics.json

This local smoke validates the Mofka append path and metrics plumbing. It does not satisfy the final distributed bare-metal benchmark requirement.
