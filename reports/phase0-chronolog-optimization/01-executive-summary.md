# Executive Summary

## Six-day progress

Over the six-day Phase 0 run, the work moved from setup and harness validation into a complete measurement/reporting loop:

- Built a common benchmark pipeline for ChronoLog, Kafka, and Mofka.
- Preserved Kafka and Mofka as fixed baselines.
- Added ChronoLog-focused measurement, profiling, semantic labels, and result collection.
- Produced accepted result cells across append throughput, payload-volume sweep, append-then-catchup, and archive/storage range workflows.
- Added report/verifier plumbing so results are checked for semantic boundaries, run identity, workload volume, profile evidence, and accept/reject decisions.
- Produced a final report, manifest refresh, evolution report, and completion audit.

## Completion status

Phase 0 was completed as a measurement-pipeline goal. The completion audit found:

- `203` accepted metric rows.
- `102` accepted manifest cells.
- `0` blocked manifest cells.
- `0` accepted rows missing the required semantic fields.
- Coverage across ChronoLog, Kafka, and Mofka.
- Coverage across 2, 4, and 8 node runs.
- Coverage across 1 KiB, 4 KiB, 16 KiB, and 64 KiB payload sizes.

## Headline result

The most useful outcome is not a single benchmark number. The main result is that the team now has a semantics-aware measurement machine that can separate:

- memory/live append paths,
- durable append paths,
- Keeper-local journal append paths,
- tail catch-up paths,
- archive/storage range retrieval paths,
- Kafka producer/consumer paths,
- Mofka memory and PMDK paths.

This distinction matters because mixing those semantics makes the numbers look cleaner but less correct.

## Important caveats

- Many accepted cells are single validated rows, not statistical distributions.
- Kafka/Mofka catch-up rows are not identical to ChronoLog archive subrange retrieval.
- ChronoLog archive/storage range throughput must use the archive/range workflow field, not a readback-only throughput field unless the chart is explicitly readback-only.
- The `10,566.9 ops/s` ChronoLog 8-node 64 KiB row is the Keeper-local journal group-commit fdatasync append row. The separate `1,868 ops/s` number refers to an active archive/range workflow after the metadata range-selection improvement, not the same workflow.

## Next recommended work

Before making larger design claims, repeat the selected headline cells with multiple trials and lock in a small canonical dashboard:

- 8-node 1 KiB durable append.
- 8-node 64 KiB durable append.
- 8-node 64 KiB tail catch-up.
- 8-node 64 KiB archive/storage range.
- Kafka acks=0 and acks=all baselines.
- Mofka memory and PMDK baselines.
