# Final Figure Grid Runbook

This runbook defines the next concrete benchmark pass needed to satisfy the stricter final figure request.

The pushed review report already covers the Phase 0 measurement pipeline and headline results. The full requested figure grid still requires new runs for 1, 2, 4, 5, and 16 nodes, all requested payload sizes, high client counts, and sync/async semantics.

## Runner

Use:

```bash
.agent/scripts/phase0_requested_figure_grid.sh
```

By default the script runs in dry-run mode and writes generated matrix commands under:

```text
.agent/results/YYYYMMDD-HHMMSS-requested-final-figure-grid/
```

To launch actual jobs:

```bash
DRY_RUN=0 TRIALS=3 CLIENTS_PER_NODE=8 \
  MOFKA_PMDK_STORAGE_TARGET_SIZE=3221225472 \
  .agent/scripts/phase0_requested_figure_grid.sh
```

For a one-cell smoke dry-run:

```bash
SMOKE=1 .agent/scripts/phase0_requested_figure_grid.sh
```

## Matrix dimensions

Default dimensions:

| Dimension | Values |
|---|---|
| Systems | ChronoLog, Kafka, Mofka |
| Node counts | 1, 2, 4, 5, 16 |
| Payload sizes | 1 KiB, 4 KiB, 16 KiB, 64 KiB |
| Client process policy | `total_clients = node_count * CLIENTS_PER_NODE` |
| Default clients per node | 8 |
| Trials | 1 by default, 3 for headline-repeat runs |
| Mofka PMDK storage target size | 3 GiB by default for requested-grid rows |

Size-aware operation counts:

| Payload | Operations per client |
|---|---:|
| 1 KiB | 40,000 |
| 4 KiB | 20,000 |
| 16 KiB | 10,000 |
| 64 KiB | 2,500 |

The operation counts are intentionally per client. Total messages scale with client count.

## Semantic batches

ChronoLog:

- append sync: Keeper-local journal group-commit tail-only fdatasync boundary.
- append async/WAL-drain: Keeper-local journal async-drain variants.
- archive/range: raw-blob archive range with async publish disabled/enabled.
- 1-node distributed ChronoLog rows are currently marked as a harness gap because the wrapper requires at least 2 nodes. For final figures, either add a true 1-node deployment path or report the row as pending harness support.

Kafka:

- append with `acks=0` and `acks=all`.
- range/catch-up with `acks=0` and `acks=all`.
- 1-node distributed Kafka rows are currently marked as a harness gap because the wrapper requires at least 2 nodes. For final figures, either add a true 1-node deployment path or report the row as pending harness support.

Mofka:

- append with four explicit variants: memory/no-flush, memory/wait-flush, PMDK/no-flush, and PMDK/wait-flush.
- range/catch-up with two explicit variants: memory/after-loop/no-flush and PMDK/per-event/wait-flush.
- PMDK rows pass `--mofka-storage-target-sizes` explicitly. The matrix default is 64 MiB, which is not large enough for the high-volume requested rows.
- PMDK per-event rows use process-isolated client execution to avoid the native-client crash observed when eight Python threads shared one Mofka client process.

## Required acceptance checks

Before these figures are promoted into the final report:

- Every row must write `metrics.json`.
- Every row must include `system`, `workflow`, `node_count`, `client_count`, `message_size_bytes`, `operation_count`, `throughput_ops_per_sec`, and `success`.
- Every row must have explicit semantic labels.
- ChronoLog archive/range rows must validate archive event count and readback.
- Kafka and Mofka rows must remain fixed baselines with no source modifications.
- Any failed row must be captured with the failure evidence, fixed when feasible, and rerun. It must not be silently dropped.
- The regenerated report must include the complete prompt-to-artifact checklist.

## Current decision

Do not mark the stricter final figure-grid objective complete until this matrix has run, passed acceptance checks, and been summarized in a regenerated final report.
