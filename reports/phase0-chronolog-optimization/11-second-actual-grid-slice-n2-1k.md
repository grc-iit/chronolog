# Actual Requested-grid Slices: 1-node and 2-node 1 KiB

This note records non-dry-run attempts against the stricter requested final figure grid.

## 1-node ChronoLog harness validation and rows

The earlier review package treated 1-node ChronoLog and Kafka rows as a harness gap. That is now corrected at the harness level:

- ChronoLog 1-node colocates Visor, Keeper, Grapher, Player, and clients on one allocated node.
- Kafka 1-node colocates ZooKeeper and a single broker on one selected node.
- The ChronoLog launcher default install was aligned to `.agent/install-tau/chronolog`, because the older `.agent/install-consistent/chronolog` binary did not contain the stop-retire grace implementation.

Smoke evidence:

```text
.agent/results/20260519-231103-n1-chronolog-append-smoke/
.agent/results/20260519-231320-n1-kafka-append-smoke/
.agent/results/20260519-231422-n1-kafka-range-smoke/
.agent/results/20260519-232347-n1-chronolog-archive-range-smoke-tau-install/
```

The final ChronoLog archive/range smoke above archived/read back `200/200` events with zero Grapher orphan chunks and confirmed the 5s stop-retire grace logs from the active install.

High-volume 1-node ChronoLog 1 KiB artifact roots:

```text
.agent/results/20260519-232619-requested-final-grid-actual-n1-1k-chronolog-append/
.agent/results/20260519-233000-requested-final-grid-actual-n1-1k-chronolog-append-async/
.agent/results/20260519-233358-requested-final-grid-actual-n1-1k-chronolog-archive/
```

| System | Workflow | Semantics | Nodes | Clients | Size | Ops/client | Total events | Throughput ops/s | Duration s |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| ChronoLog | append_throughput | sync, keeper journal group-commit tail-only fdatasync | 1 | 8 | 1 KiB | 40,000 | 320,000 | 2,449.370 | 130.646 |
| ChronoLog | append_throughput | async WAL drain, variant 1 | 1 | 8 | 1 KiB | 40,000 | 320,000 | 29,429.500 | 10.873 |
| ChronoLog | append_throughput | async WAL drain, variant 2 | 1 | 8 | 1 KiB | 40,000 | 320,000 | 30,021.600 | 10.659 |

| System | Workflow | Semantics | Nodes | Clients | Size | Ops/client | Archive events | Readback events | Active throughput ops/s | Readback throughput ops/s | Duration s |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ChronoLog | archive_range_retrieval | raw-blob archive/range, sync publish, 5s Grapher stop-retire grace | 1 | 8 | 1 KiB | 40,000 | 320,000 | 320,000 | 5,259.718 | 121,842.111 | 63.342 |
| ChronoLog | archive_range_retrieval | raw-blob archive/range, async publish x4, 5s Grapher stop-retire grace | 1 | 8 | 1 KiB | 40,000 | 320,000 | 320,000 | 5,414.744 | 107,561.017 | 61.602 |

Validation:

```text
python3 .agent/scripts/phase0_validate_metrics.py $(find .agent/results/20260519-232619-requested-final-grid-actual-n1-1k-chronolog-append -name metrics.json | sort)
python3 .agent/scripts/phase0_validate_metrics.py $(find .agent/results/20260519-233000-requested-final-grid-actual-n1-1k-chronolog-append-async -name metrics.json | sort)
python3 .agent/scripts/phase0_validate_metrics.py $(find .agent/results/20260519-233358-requested-final-grid-actual-n1-1k-chronolog-archive -name metrics.json | sort)
```

All five high-volume ChronoLog 1-node metrics passed validation.

High-volume 1-node Kafka 1 KiB artifact root:

```text
.agent/results/20260519-234135-requested-final-grid-actual-n1-1k-kafka/
```

| System | Workflow | Semantics | Nodes | Clients | Size | Ops/client | Total events | Throughput ops/s | Duration s | p50 ms | p95 ms | p99 ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Kafka | append_throughput | `acks=0` | 1 | 8 | 1 KiB | 40,000 | 320,000 | 31,295.965 | 10.225 | 3,388.000 | 4,048.000 | 4,118.000 |
| Kafka | append_throughput | `acks=all` RF1 | 1 | 8 | 1 KiB | 40,000 | 320,000 | 29,401.080 | 10.884 | 3,952.000 | 4,539.000 | 4,696.000 |
| Kafka | range_retrieval | append first with `acks=0`, then consumer catch-up | 1 | 8 | 1 KiB | 40,000 | 320,000 | 50,235.479 | 6.370 | null | null | null |
| Kafka | range_retrieval | append first with `acks=all`, then consumer catch-up | 1 | 8 | 1 KiB | 40,000 | 320,000 | 49,162.698 | 6.509 | null | null | null |

Validation:

```text
python3 .agent/scripts/phase0_validate_metrics.py $(find .agent/results/20260519-234135-requested-final-grid-actual-n1-1k-kafka -name metrics.json | sort)
```

All four high-volume Kafka 1-node metrics passed validation.

High-volume 1-node Mofka 1 KiB artifact roots:

```text
.agent/results/20260519-181525-requested-final-grid-actual-n1-1k-mofka/
.agent/results/20260519-191902-requested-final-grid-actual-n1-1k-mofka-pmdk-none/
.agent/results/20260519-192639-requested-final-grid-actual-n1-1k-mofka-pmdk-wait-flush-processes/
.agent/results/20260519-235231-requested-final-grid-actual-n1-1k-mofka-range-memory-clientgroup/
.agent/results/20260519-235721-requested-final-grid-actual-n1-1k-mofka-range-pmdk-clientgroup/
```

| System | Workflow | Semantics | Nodes | Clients | Size | Ops/client | Total events | Throughput ops/s | Duration s | p50 ms | p95 ms | p99 ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Mofka | append_throughput | memory partition, no producer wait, no flush | 1 | 8 | 1 KiB | 40,000 | 320,000 | 14,207.755 | 22.523 | null | null | null |
| Mofka | append_throughput | memory partition, per-event wait, after-loop flush | 1 | 8 | 1 KiB | 40,000 | 320,000 | 10,426.057 | 30.692 | 0.188 | 0.410 | 0.622 |
| Mofka | append_throughput | PMDK/default partition, no producer wait, no flush | 1 | 8 | 1 KiB | 40,000 | 320,000 | 2,853.392 | 112.147 | null | null | null |
| Mofka | append_throughput | PMDK/default partition, per-event wait, after-loop flush, process clients | 1 | 8 | 1 KiB | 40,000 | 320,000 | 74.264 | 4,308.952 | 107.125 | 108.498 | 110.501 |
| Mofka | range_retrieval | memory partition, after-loop wait, no flush, then consumer pull catch-up | 1 | 8 | 1 KiB | 40,000 | 320,000 | 6,906.989 | 46.330 | 0.034 | 0.087 | 0.171 |
| Mofka | range_retrieval | PMDK/default partition, per-event wait, after-loop flush, then consumer pull catch-up | 1 | 8 | 1 KiB | 40,000 | 320,000 | 2,658.740 | 120.358 | 0.223 | 0.347 | 0.437 |

The first 1-node Mofka memory range attempt failed before metrics because the Flock group file exposed duplicate/out-of-order members (`storage, storage, master`). The harness now writes a client-facing `mofka-client.json` from `bedrock-query-before-benchmark.json`, preserving the service group while ordering unique endpoints master-first for the Mofka client setup path.

Validation:

```text
python3 .agent/scripts/phase0_validate_metrics.py .agent/results/20260519-235231-requested-final-grid-actual-n1-1k-mofka-range-memory-clientgroup/001-mofka-range_retrieval-memory-memory-after_loop-no_flush-n1-c8-s1024-o40000-t1/mofka/metrics.json
python3 .agent/scripts/phase0_validate_metrics.py .agent/results/20260519-235721-requested-final-grid-actual-n1-1k-mofka-range-pmdk-clientgroup/001-mofka-range_retrieval-default-pmdk-per_event-after_loop-n1-c8-s1024-o40000-t1/mofka/metrics.json
```

Both high-volume Mofka 1-node range/catch-up metrics passed validation. The memory row is live-memory evidence; the PMDK/default row is storage-backed Mofka fixed-baseline evidence, but still a consumer catch-up semantic rather than a ChronoLog archive subrange equivalent.

## 2-node ChronoLog and Kafka rows

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

This report now adds usable ChronoLog 1-node and 2-node 1 KiB append numbers for sync versus async semantics, usable ChronoLog 1-node and 2-node 1 KiB archive/range rows with the 5s Grapher stop-retire grace, and usable Kafka 1-node and 2-node 1 KiB append/range rows. It does not satisfy the final requested-grid objective because the remaining 4K/16K/64K payload sizes and 4/5/16-node requested-grid cells still need staged execution across systems.
