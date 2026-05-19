# ChronoLog Changes and Decisions

Phase 0 accepted only bounded ChronoLog changes that improved measurement, semantic clarity, or a specific validated workflow. Kafka and Mofka were not modified.

## Accepted areas

- Keeper-local durable append path with group-commit/fdatasync semantics and 8-node scaling evidence.
- Packed Keeper cursor bulk tail catch-up.
- Auto-vectored tail-read policy.
- Per-Keeper skew reporting.
- Archive manifest event-time range index for archive/range metadata selection.
- Raw-blob archive layout experiments.
- Async close/publish experiments where accepted by evidence.
- Archive stage-attribution tooling.
- Grapher stop-retire attribution.
- Outside-lock drain-complete wait evidence where the scoped result held.
- Report/verifier plumbing for semantic labels, run identity, profile-artifact gates, workload-volume gates, and accept/reject visibility.

## Rejected or default-off examples

Some experiments were preserved as evidence but not accepted as default behavior.

The clearest example is Keeper async drain-complete. It improved some stop/release timing but regressed matched 1 KiB range throughput, so it remained default-off.

## Paper alignment

The current design is closer to the paper in that Keeper-local journaling, archive/range selection, and retrieval semantics are now visible and measurable. It is not a full paper-equivalent implementation. Locking, coordination, and implementation-specific synchronization still exist. Phase 0 made those areas observable so the next phase can target them with evidence instead of guessing.

## What changed operationally

The important operational improvement is the ability to answer:

- which workflow was run,
- which semantic boundary it represents,
- which storage backend was involved,
- whether the row is accepted or rejected,
- which artifact paths support it,
- whether profile evidence exists,
- whether the run had enough workload volume to be meaningful.
