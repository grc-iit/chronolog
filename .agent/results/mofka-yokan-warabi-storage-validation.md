# Mofka Yokan/Warabi Storage Validation

Status: fixed and validated.

The Mofka benchmark driver now supports `--partition-type default`, which creates the topic with Mofka's default partition path and configured Yokan metadata plus Warabi data providers instead of the memory-only partition path.

Validation runs:

| Workflow | Result | Evidence |
|---|---|---|
| append_throughput | success | `.agent/results/20260512-091538/mofka/metrics.json` |
| range_retrieval | success | `.agent/results/20260512-093629/mofka/metrics.json` |

Both result directories include `config/mofka-workload.json` with `partition_type: default` and provider fields for the Yokan/Warabi-backed storage path.
