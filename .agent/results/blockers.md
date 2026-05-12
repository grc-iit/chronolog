# Phase 0 Blockers

STOP_RALPH_LOOP

## ChronoLog Distributed Range Retrieval

Status: blocking Phase 0 completion.

Phase 0 requires the selected workflows to run on ChronoLog, Kafka, and Mofka. The provisional selected suite includes `range_retrieval` when all systems expose a comparable read path. Kafka and Mofka now have distributed range/read evidence, but ChronoLog's distributed `ReplayStory` path failed twice.

### Attempt 1

- Result directory: `.agent/results/20260512-011348/`
- Command path: `.agent/scripts/chronolog_run_append_distributed.sh --workflow range_retrieval`
- Outcome: `ReplayStory` did not return before the SLURM allocation expired.
- Secondary effect: cleanup then lost compute-node SSH access because Ares `pam_slurm_adopt` requires an active job.

### Attempt 2

- Result directory: `.agent/results/20260512-012853/`
- Command path: `.agent/scripts/chronolog_run_append_distributed.sh --workflow range_retrieval`
- Harness change: internal `timeout 300s` around the Python `ReplayStory` client.
- Outcome: timeout expired before `ReplayStory` returned; cleanup completed before allocation expiry.
- ChronoVisor evidence: `.agent/results/20260512-012853/chronolog/logs/chrono-visor-ares-comp-03.launch.log`

Relevant log excerpt:

```text
Function returned HG_NOENTRY
terminate called after throwing an instance of 'thallium::margo_exception'
what(): [margo_respond] Function returned HG_NOENTRY
```

## Impact

- `range_retrieval` is not complete across all three systems.
- `mixed_append_read` is not safe to claim because the ChronoLog read side is blocked.
- `All selected workflows run on ChronoLog distributed target` remains incomplete.
- `Results are comparable across systems` remains incomplete for the full selected suite.

## Non-blocking Open Issues

- Mofka Yokan/Warabi-backed default partition configuration still needs to be fixed, but Mofka memory-partition distributed append/read smokes work.
- `perf` and eBPF-based observability need admin/capability changes for full low-level profiling, but those do not block the benchmark harness itself.
