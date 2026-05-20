# Second Actual Requested-grid Slice: 2-node 1 KiB

This note records the second non-dry-run attempt against the stricter requested final figure grid.

ChronoLog artifact root:

```text
.agent/results/20260519-183508-requested-final-grid-actual-n2-1k/
```

Kafka artifact root:

```text
.agent/results/20260519-190113-requested-final-grid-actual-n2-1k-kafka/
```

Kafka retry artifact root:

```text
.agent/results/20260519-191059-requested-final-grid-actual-n2-1k-kafka-retry/
```

## Command shapes

ChronoLog requested-grid slice:

```bash
NODES=2 SIZES=1024 DRY_RUN=0 CLIENTS_PER_NODE=8 TRIALS=1 SLURM_TIME=01:30:00 \
  RESULT_ROOT=.agent/results/20260519-183508-requested-final-grid-actual-n2-1k \
  .agent/scripts/phase0_requested_figure_grid.sh
```

First Kafka-only retry:

```bash
python3 .agent/scripts/phase0_benchmark_matrix.py \
  --partition datacrumbs \
  --slurm-time 01:00:00 \
  --trials 1 \
  --result-dir .agent/results/20260519-190113-requested-final-grid-actual-n2-1k-kafka \
  --systems kafka \
  --workflows append_throughput,range_retrieval \
  --kafka-acks-values 0,all \
  --node-counts 2 \
  --message-sizes 1024 \
  --operation-counts 40000 \
  --client-counts 16
```

Second Kafka-only retry after node-probe hardening:

```bash
python3 .agent/scripts/phase0_benchmark_matrix.py \
  --partition datacrumbs \
  --slurm-time 01:00:00 \
  --trials 1 \
  --result-dir .agent/results/20260519-191059-requested-final-grid-actual-n2-1k-kafka-retry \
  --systems kafka \
  --workflows append_throughput,range_retrieval \
  --kafka-acks-values 0,all \
  --node-counts 2 \
  --message-sizes 1024 \
  --operation-counts 40000 \
  --client-counts 16
```

## Completed ChronoLog append rows

| System | Workflow | Semantics | Nodes | Clients | Size | Ops/client | Throughput ops/s | Duration s |
|---|---|---|---:|---:|---:|---:|---:|---:|
| ChronoLog | append_throughput | sync, keeper journal group-commit tail-only fdatasync | 2 | 16 | 1 KiB | 40,000 | 5,927.970 | 107.963 |
| ChronoLog | append_throughput | async WAL drain, 1 drain worker | 2 | 16 | 1 KiB | 40,000 | 55,059.700 | 11.624 |
| ChronoLog | append_throughput | async WAL drain, 4 drain workers | 2 | 16 | 1 KiB | 40,000 | 52,227.300 | 12.254 |

Each row used 16 clients across 2 nodes and 40,000 operations per client, for 640,000 total append operations.

## Completed ChronoLog archive/range rows

The initial ChronoLog archive/range rows failed before writing `metrics.json`.

Observed stderr summaries:

```text
archive/range row 001: timed out waiting for archive event count; expected 40000, last_count 12700
archive/range row 002: timed out waiting for archive event count; expected 40000, last_count 15533
```

Follow-up reruns found two separate issues:

1. The ChronoLog distributed wrapper was dropping archive/range timeout knobs across its outer SLURM self-resubmission step. The matrix command requested `--chronolog-archive-wait-seconds 2400`, `--chronolog-archive-range-timeout-seconds 3600`, and `--chronolog-archive-range-event-counts 40000`, but the generated recursive `sbatch` script reverted to `--archive-wait-seconds 420`, `timeout 1200s`, and `--range-event-count 0`.
2. After fixing that forwarding gap, both corrected long-wait variants still failed downstream archive completeness for the 2-node, 16-client, 1 KiB, 40,000 ops/client shape.

Corrected long-wait artifact root:

```text
.agent/results/20260519-212132-requested-final-grid-actual-n2-1k-chronolog-archive-longwait-forwarded/
```

Corrected generated `sbatch` evidence includes:

```text
--archive-wait-seconds 2400
--archive-range-timeout-seconds 3600
--archive-range-event-count 40000
```

Corrected long-wait stderr summaries:

```text
sync raw-blob publish: expected 40000 archived events for story 14, last_count 10237, poll_count 2328
async raw-blob publish x4: expected 40000 archived events for story 14, last_count 18879, poll_count 2342
```

All 16 append clients in each corrected row reached `release_story_returned`, but Grapher logs showed late chunks arriving after story retirement and being recorded as orphan chunks. A bounded 5s Grapher stop-retire grace was then tested on the same 2-node, 16-client, 1 KiB, 40,000 ops/client cell.

Grace artifact root:

```text
.agent/results/20260519-225156-requested-final-grid-actual-n2-1k-chronolog-archive-grace5s/
```

The 5s grace rerun completed both archive/range variants, passed metrics validation, archived and read back all 640,000 events, and produced zero Grapher orphan chunks.

| System | Workflow | Semantics | Nodes | Clients | Size | Ops/client | Archive events | Readback events | Active throughput ops/s | Readback throughput ops/s | Duration s |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ChronoLog | archive_range_retrieval | raw-blob archive/range, sync publish, 5s Grapher stop-retire grace | 2 | 16 | 1 KiB | 40,000 | 640,000 | 640,000 | 5,270.966 | 128,736.431 | 123.927 |
| ChronoLog | archive_range_retrieval | raw-blob archive/range, async publish x4, 5s Grapher stop-retire grace | 2 | 16 | 1 KiB | 40,000 | 640,000 | 640,000 | 5,364.442 | 136,898.966 | 121.942 |

Validation:

```text
python3 .agent/scripts/phase0_validate_metrics.py $(find .agent/results/20260519-225156-requested-final-grid-actual-n2-1k-chronolog-archive-grace5s -name metrics.json | sort)
```

Both ChronoLog archive/range metrics passed validation.

## Kafka retry results

The first Kafka-only retry did not produce a benchmark metric. It stalled before benchmark execution while SLURM waited on an unavailable requested node:

```text
ReqNodeNotAvail, UnavailableNodes:ares-comp-07
```

The Kafka wrapper was then hardened to probe candidate nodes with `srun --immediate` and skip nodes that cannot launch promptly. The second retry skipped `ares-comp-07`, selected `ares-comp-06` and `ares-comp-08`, and completed all four Kafka rows.

| System | Workflow | Semantics | Nodes | Clients | Size | Ops/client | Throughput ops/s | Duration s | p50 ms | p95 ms | p99 ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Kafka | append_throughput | `acks=0` | 2 | 16 | 1 KiB | 40,000 | 28,391.230 | 22.542 | 7,995.000 | 8,825.000 | 8,925.000 |
| Kafka | append_throughput | `acks=all` RF1 | 2 | 16 | 1 KiB | 40,000 | 24,689.502 | 25.922 | 9,420.000 | 11,190.000 | 11,387.000 |
| Kafka | range_retrieval | append first with `acks=0`, then consumer catch-up | 2 | 16 | 1 KiB | 40,000 | 83,019.847 | 7.709 | null | null | null |
| Kafka | range_retrieval | append first with `acks=all`, then consumer catch-up | 2 | 16 | 1 KiB | 40,000 | 83,160.083 | 7.696 | null | null | null |

Validation:

```text
python3 .agent/scripts/phase0_validate_metrics.py $(find .agent/results/20260519-191059-requested-final-grid-actual-n2-1k-kafka-retry -name metrics.json | sort)
```

All four Kafka metrics passed validation.

## Decision

This slice adds usable ChronoLog 2-node 1 KiB append numbers for sync versus async semantics, usable ChronoLog 2-node 1 KiB archive/range rows with the 5s Grapher stop-retire grace, and usable Kafka 2-node 1 KiB append/range rows. It does not satisfy the final requested-grid objective because the remaining 4K/16K/64K payload sizes and 4/5/16-node requested-grid cells still need staged execution.
