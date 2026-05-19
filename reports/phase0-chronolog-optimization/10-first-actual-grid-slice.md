# First Actual Requested-grid Slice

This note records the first non-dry-run attempt against the stricter requested final figure grid.

Artifact root:

```text
.agent/results/20260519-181525-requested-final-grid-actual-n1-1k-mofka/
```

Command shape:

```bash
SMOKE=1 DRY_RUN=0 CLIENTS_PER_NODE=8 TRIALS=1 SLURM_TIME=00:45:00 \
  RESULT_ROOT=.agent/results/20260519-181525-requested-final-grid-actual-n1-1k-mofka \
  .agent/scripts/phase0_requested_figure_grid.sh
```

## Completed rows

| System | Workflow | Semantics | Nodes | Clients | Size | Ops/client | Throughput ops/s | Duration s | p50 ms | p95 ms | p99 ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Mofka | append_throughput | memory none/no-flush | 1 | 8 | 1 KiB | 40,000 | 14,207.755 | 22.523 | null | null | null |
| Mofka | append_throughput | memory per-event wait + after-loop flush | 1 | 8 | 1 KiB | 40,000 | 10,426.057 | 30.692 | 0.188 | 0.410 | 0.622 |

Each row used 8 clients on 1 node and 40,000 operations per client, for 320,000 total events.

## Skipped rows

The runner skipped the ChronoLog and Kafka 1-node distributed rows because those distributed harnesses require at least 2 nodes.

## Blocked row

The Mofka PMDK/no-flush 1-node 1 KiB high-volume row was manually terminated after it ran for more than 15 minutes without completing. At inspection time, clients were only around 7.2k of 40k events each. The row did not produce `metrics.json` and should be treated as blocked for this high-volume 1-node setting until the PMDK run shape is adjusted or given a much larger walltime.

## Decision

This actual run validates that the requested-grid runner can collect real rows, but it also shows that the full high-volume grid needs staged execution and row-specific walltime/blocker handling. Do not treat this as completion of the final figure objective.
