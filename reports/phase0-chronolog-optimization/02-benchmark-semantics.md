# Benchmark Semantics

The Phase 0 reports deliberately label every accepted row with semantic boundaries. This prevents memory-only, durable, catch-up, and archive/range workflows from being compared as if they were the same operation.

## Main workflow classes

| Class | Meaning | Example rows |
|---|---|---|
| Memory/live append | Append accepted before durable storage semantics | Mofka memory no-flush |
| Durable append | Append includes durable or flush-related semantics | ChronoLog Keeper-local journal fdatasync, Mofka PMDK flush variants |
| Consumer/tail catch-up | Read recent appended data through a live or tail path | ChronoLog Keeper cursor tail catch-up, Kafka consumer catch-up, Mofka pull catch-up |
| Archive/storage range | Read persisted data through an archive/storage range path | ChronoLog archive/storage range, Mofka PMDK range/catch-up |

## Why the split matters

A memory-only append row and a durable append row do not answer the same question. A tail catch-up row and an archive subrange row also do not answer the same question. The report therefore keeps these workflows separate and records fields such as:

- `semantic_boundary`
- `append_ack_boundary`
- `durability_boundary`
- `storage_backend`

The completion audit found zero accepted rows missing these required semantic fields.

## Sync versus async wording

For the ChronoLog rows discussed in the final report:

- `Keeper-local journal group-commit fdatasync` is a durable append class.
- `Keeper cursor packed bulk auto-vectored tail catch-up` is a tail catch-up class.
- `archive/storage range` rows are storage/range retrieval class.

The async work should be read carefully. The accepted archive/range improvement reduced metadata selection cost for range queries. It did not turn the entire system into a fully paper-matching asynchronous design. Some rejected/default-off async or outside-lock experiments are preserved as evidence, but not all are accepted as default behavior.

## Paper alignment

The implementation moved closer to the paper in the areas that matter for measurement:

- Keeper-local journal semantics are explicit.
- Archive/range retrieval is measured separately from live append.
- Metadata/range-selection costs are visible.
- Stop/release and drain-complete attribution is measured.

It is still not a pure paper design. There is still locking, coordination, and implementation-specific control flow. Phase 0 established where those costs are visible and measurable; it did not complete a storage-layout or concurrency redesign.
