# Requested Figure Grid Gap Audit

This audit checks the current pushed review branch against the stricter requested figure set:

> sync vs async, append, retrieve and other semantics, 1/2/4/5/16 nodes, ChronoLog/Kafka/Mofka, 1 KiB/4 KiB/16 KiB/64 KiB, high message counts, and high client process counts across nodes and high ppn, for the final Phase 0 report.

## Current status

The current review branch is ready for engineering review of the completed Phase 0 measurement pipeline, code changes, benchmark harness, and headline results.

It is not a complete final figure grid for the stricter request above.

## Prompt-to-artifact checklist

| Requirement | Current evidence | Status |
|---|---|---|
| ChronoLog figures | Report includes ChronoLog append, tail catch-up, archive/range headline rows and ChronoLog code/harness changes. | Partial |
| Kafka figures | Report includes Kafka acks=0 and acks=all append/catch-up headline rows. Kafka remains a fixed baseline. | Partial |
| Mofka figures | Report includes Mofka memory and PMDK append/catch-up headline rows. Mofka remains a fixed baseline. | Partial |
| Append workflow | 8-node 1 KiB and 64 KiB append rows are in `03-results-summary.md`; broader size coverage is mentioned in the audit. | Partial |
| Retrieve workflow | 8-node 64 KiB tail catch-up and archive/storage range rows are in `03-results-summary.md`. | Partial |
| Other semantics | Memory/live, durable append, tail catch-up, and archive/range semantics are documented in `02-benchmark-semantics.md`. | Partial |
| Sync vs async figures | Some ChronoLog async/default-off experiments are documented, and Kafka/Mofka durability modes are represented, but there is no complete normalized sync-vs-async figure matrix. | Missing |
| Node counts 1/2/4/5/16 | Current completed audit says coverage includes 2, 4, and 8 nodes. The explicit 1, 5, and 16 node requested grid is not complete. | Missing |
| Payload sizes 1 KiB/4 KiB/16 KiB/64 KiB | Current audit says accepted rows cover all four payload sizes, but the pushed report only lists headline 1 KiB and 64 KiB rows. Full per-size figure tables are not present in this branch. | Partial |
| High message counts | Some headline append rows use 160,000 messages for 1 KiB and 40,000 messages for 64 KiB; retrieval examples use 10,000 messages. This is not yet a consistent high-volume grid. | Partial |
| High client process counts | Current report includes client count in raw metrics schema and benchmark scripts, but no final high-ppn/high-client-count matrix is published. | Missing |
| Final Phase 0 report | Report folder exists and is pushed, but it is a review report, not the complete requested figure-grid report. | Partial |

## Missing runs for the requested final figure set

To complete the stricter figure request, the next benchmark matrix should produce accepted rows for:

- Systems: ChronoLog, Kafka, Mofka.
- Workflows: append, retrieve/catch-up, archive/range where semantically valid.
- Semantics:
  - ChronoLog synchronous/durable Keeper-local journal path.
  - ChronoLog async/default-off variants only if they pass guardrails.
  - Kafka acks=0 and acks=all.
  - Mofka memory/no-flush and PMDK flush variants.
- Node counts: 1, 2, 4, 5, 16.
- Payload sizes: 1 KiB, 4 KiB, 16 KiB, 64 KiB.
- Client/process layout: explicit client process count, ppn, total clients, and per-node distribution.
- Message volume: fixed high-volume policy per payload size so initialization does not dominate.
- Repeats: at least enough repeated headline rows to avoid presenting single-run noise as final performance.

## Recommended next matrix

Use the existing benchmark harness as the base, but create a new run manifest with explicit dimensions:

```text
systems = chronolog,kafka,mofka
workflows = append_throughput,append_then_catchup,archive_storage_range
node_counts = 1,2,4,5,16
payload_sizes = 1024,4096,16384,65536
client_ppn = high but cluster-safe, recorded per run
operation_count = high enough per payload size to keep initialization below the noise floor
repeats = 3 for headline cells, 1 for exploratory cells
```

The matrix should preserve semantic boundaries rather than collapsing all rows into a single leaderboard.

## Completion decision

The stricter figure-grid objective is not complete yet. The current artifacts are sufficient for engineering review and PR discussion, but the requested final figure set still needs additional benchmark runs and a regenerated report.
