# Results Summary

These are headline rows from the final Phase 0 report. Treat them as accepted measurement rows, not as statistically final claims.

## 8-node append throughput

| System | Semantics | Size | Throughput ops/s | Messages | Payload bytes |
|---|---|---:|---:|---:|---:|
| ChronoLog | Keeper-local journal group-commit fdatasync | 1 KiB | 100,715.000 | 160,000 | 163,840,000 |
| ChronoLog | Keeper-local journal group-commit fdatasync | 64 KiB | 10,566.900 | 40,000 | 2,621,440,000 |
| Kafka | acks=0 | 1 KiB | 32,202.047 | 160,000 | 163,840,000 |
| Kafka | acks=0 | 64 KiB | 2,070.717 | 40,000 | 2,621,440,000 |
| Kafka | acks=all RF1 | 1 KiB | 28,597.634 | 160,000 | 163,840,000 |
| Kafka | acks=all RF1 | 64 KiB | 1,991.969 | 40,000 | 2,621,440,000 |
| Mofka | memory none/no-flush | 1 KiB | 7,746.287 | 160,000 | 163,840,000 |
| Mofka | memory none/no-flush | 64 KiB | 2,657.494 | 40,000 | 2,621,440,000 |
| Mofka | PMDK per-event wait + after-loop flush | 1 KiB | 935.007 | 160,000 | 163,840,000 |
| Mofka | PMDK per-event wait + after-loop flush | 64 KiB | 758.145 | 40,000 | 2,621,440,000 |

## 8-node catch-up and range retrieval

| System | Semantics | Size | Throughput ops/s | Messages | Payload bytes |
|---|---|---:|---:|---:|---:|
| ChronoLog | Keeper cursor packed bulk auto-vectored tail catch-up | 64 KiB | 16,668.328 | 10,000 | 655,360,000 |
| Kafka | acks=0 consumer catch-up | 64 KiB | 1,901.141 | 10,000 | 655,360,000 |
| Kafka | acks=all RF1 consumer catch-up | 64 KiB | 1,871.958 | 10,000 | 655,360,000 |
| Mofka | memory after-loop/no-flush pull catch-up | 64 KiB | 3,790.944 | 10,000 | 655,360,000 |
| Mofka | PMDK storage-backed pull catch-up | 64 KiB | 2,645.110 | 10,000 | 655,360,000 |
| ChronoLog | archive/storage range baseline | 64 KiB | 1,198.511 | 10,000 | 655,360,000 |
| ChronoLog | archive/storage range manifest index row | 64 KiB | 1,708.502 | 10,000 | 655,360,000 |
| Mofka | PMDK storage-backed range/catch-up | 64 KiB | 2,645.110 | 10,000 | 655,360,000 |

## Clarifying the 10,566.9 versus 1,868 numbers

The `10,566.9 ops/s` row is ChronoLog 8-node 64 KiB append throughput with Keeper-local journal group-commit fdatasync semantics.

The approximately `1,868 ops/s` number belongs to a different ChronoLog workflow: an active archive/range workflow after improving metadata range-selection. In that accepted archive/range improvement, metadata range-selection dropped from about `1.44s` to about `0.021s` on the 8-node 64 KiB archive row, while active workflow throughput moved from about `1,734` to `1,868 ops/s`.

Those two numbers should not be compared as if they are the same benchmark.
