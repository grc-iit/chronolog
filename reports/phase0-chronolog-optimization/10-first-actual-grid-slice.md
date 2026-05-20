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

## 1-node harness gap

The runner skipped the ChronoLog and Kafka 1-node distributed rows because the current distributed wrappers exit with a minimum 2-node guard. This should be treated as a harness/deployment gap for the requested final figure grid, not proof that ChronoLog or Kafka cannot run on one node.

## Open row needing rerun

The Mofka PMDK/no-flush 1-node 1 KiB high-volume row was manually terminated after it ran for more than 15 minutes without completing. That was too aggressive for a final benchmark: slow PMDK behavior is itself a valid result unless the row exceeds an explicit walltime or is clearly hung. At inspection time, clients were only around 7.2k of 40k events each. The row did not produce `metrics.json` and should be rerun with a much larger walltime or a staged PMDK-specific policy before it is used in the final figure set.

Follow-up: the requested-grid runner now passes a 3 GiB PMDK storage-target size for PMDK rows instead of relying on the matrix default. The PMDK rows still need actual reruns; this only fixes the command shape.

## PMDK rerun results

The PMDK rows were rerun with explicit 3 GiB PMDK storage targets. The no-wait/no-flush row used the normal threaded benchmark path. The per-event/wait + after-loop flush row initially segfaulted in the native client path when eight Python threads shared one process, so the benchmark harness was extended with process-isolated client execution and rerun.

Artifact roots:

```text
.agent/results/20260519-191902-requested-final-grid-actual-n1-1k-mofka-pmdk-none/
.agent/results/20260519-192639-requested-final-grid-actual-n1-1k-mofka-pmdk-wait-flush-processes/
```

| System | Workflow | Semantics | Client mode | Nodes | Clients | Size | Ops/client | Throughput ops/s | Duration s | p50 ms | p95 ms | p99 ms |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Mofka | append_throughput | PMDK none/no-flush | threads | 1 | 8 | 1 KiB | 40,000 | 2,853.392 | 112.147 | null | null | null |
| Mofka | append_throughput | PMDK per-event wait + after-loop flush | processes | 1 | 8 | 1 KiB | 40,000 | 74.264 | 4,308.952 | 107.125 | 108.498 | 110.501 |

Validation:

```text
python3 .agent/scripts/phase0_validate_metrics.py $(find .agent/results/20260519-191902-requested-final-grid-actual-n1-1k-mofka-pmdk-none -name metrics.json | sort)
python3 .agent/scripts/phase0_validate_metrics.py $(find .agent/results/20260519-192639-requested-final-grid-actual-n1-1k-mofka-pmdk-wait-flush-processes -name metrics.json | sort)
```

Both rerun metrics passed validation. The slow per-event PMDK row is accepted as a real measured result, not treated as a failure.

## Decision

This actual run validates that the requested-grid runner can collect real rows and that the PMDK rows can be completed when run with explicit storage sizing and enough walltime. Do not treat this as completion of the final figure objective.
