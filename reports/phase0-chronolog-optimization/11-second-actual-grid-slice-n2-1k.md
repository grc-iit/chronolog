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

## Command shapes

ChronoLog requested-grid slice:

```bash
NODES=2 SIZES=1024 DRY_RUN=0 CLIENTS_PER_NODE=8 TRIALS=1 SLURM_TIME=01:30:00 \
  RESULT_ROOT=.agent/results/20260519-183508-requested-final-grid-actual-n2-1k \
  .agent/scripts/phase0_requested_figure_grid.sh
```

Kafka-only retry:

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

## Completed ChronoLog append rows

| System | Workflow | Semantics | Nodes | Clients | Size | Ops/client | Throughput ops/s | Duration s |
|---|---|---|---:|---:|---:|---:|---:|---:|
| ChronoLog | append_throughput | sync, keeper journal group-commit tail-only fdatasync | 2 | 16 | 1 KiB | 40,000 | 5,927.970 | 107.963 |
| ChronoLog | append_throughput | async WAL drain, 1 drain worker | 2 | 16 | 1 KiB | 40,000 | 55,059.700 | 11.624 |
| ChronoLog | append_throughput | async WAL drain, 4 drain workers | 2 | 16 | 1 KiB | 40,000 | 52,227.300 | 12.254 |

Each row used 16 clients across 2 nodes and 40,000 operations per client, for 640,000 total append operations.

## Blocked ChronoLog archive/range rows

The same run reached ChronoLog archive/range rows, but both variants failed before writing `metrics.json`.

Observed stderr summaries:

```text
archive/range row 001: timed out waiting for archive event count; expected 40000, last_count 12700
archive/range row 002: timed out waiting for archive event count; expected 40000, last_count 15533
```

This is a real requested-grid blocker. The append rows are usable as append evidence, but archive/range cannot be promoted until the archive drain/readback path either completes or is reported as a blocked row with accepted rationale.

## Blocked Kafka retry

The Kafka-only retry did not produce a benchmark metric. It stalled before benchmark execution while SLURM waited on an unavailable requested node:

```text
ReqNodeNotAvail, UnavailableNodes:ares-comp-07
```

The local driver was stopped after confirming no `metrics.json` existed. This should be retried with a fresh allocation that does not pin to the unavailable node, or the Kafka wrapper should be adjusted to avoid reusing stale node selection.

## Decision

This slice adds usable ChronoLog 2-node 1 KiB append numbers for sync versus async semantics. It does not satisfy the final requested-grid objective because archive/range and Kafka rows remain incomplete, and the rest of the node/size matrix still needs execution.
