# ChronoLog Correctness Policy

Performance changes are acceptable only after correctness passes.

The current Phase 0 pipeline has basic build, launch, metrics, append, and archive-backed range validation. Before autonomous optimization, these checks should become explicit executable validators.

## Required Semantic Checks

- No lost records: every acknowledged append must be retrievable through the selected validation path.
- No duplicate records: retrieved records must not contain duplicate event identifiers for one story/range.
- Ordering preserved: ordering must match the ChronoLog semantics selected for the benchmark.
- Valid time-range retrieval: range queries/replay must return only records in the requested range and must include all expected records in that range.
- No benchmark oracle modification: optimization patches must not modify scoring, validators, or baseline metrics.

## Minimum Validator Output

Each validator should write:

```json
{
  "validator": "name",
  "workflow": "append_throughput|append_latency|range_retrieval|mixed_read_write|scaling_skew",
  "result": "pass|fail|blocked",
  "records_expected": 0,
  "records_observed": 0,
  "duplicates": 0,
  "lost_records": 0,
  "ordering_violations": 0,
  "evidence": ["path/to/log/or/output"]
}
```

## Current Gap

The next implementation step is to wrap existing append and range validation outputs into this explicit JSON form, then require the Performance Judge to consume it before accepting any optimization.
