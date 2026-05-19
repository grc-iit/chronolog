# Deployment and Reproduction

This report branch is a lightweight shareable view. The full raw run artifacts live on the experiment branch and in the local `.agent/results` paths listed in [data/artifact-index.md](data/artifact-index.md).

## Repository and branch policy

ChronoLog Phase 0 work was performed from a branch created from `develop`, using the `opt/phase0-*` branch pattern. ChronoLog was the only system modified.

Before making or replaying ChronoLog changes:

```bash
git branch --show-current
git merge-base --is-ancestor origin/develop HEAD
```

Do not run this work directly on `develop`, `main`, or `master`.

## Build path

The validated ChronoLog rebuild wrapper used by the harness was:

```bash
profileforge/targets/chronolog/build.sh
```

The Phase 0 runs also used generated `.agent` scripts and result directories. In the full experiment branch, each experiment writes under:

```text
.agent/results/YYYYMMDD-HHMMSS/
```

## Benchmark and report pipeline

The pipeline is organized around:

- distributed append benchmark runners,
- append-then-catchup runners,
- archive/storage range runners,
- manifest generation,
- evolution reporting,
- completion auditing.

Important generated or experiment-branch scripts include:

- `.agent/scripts/phase0_benchmark_matrix.py`
- `.agent/scripts/phase0_six_way_manifest.py`
- `.agent/scripts/phase0_evolution_report.py`
- `.agent/scripts/phase0_archive_stage_attribution.py`
- `.agent/scripts/chronolog_run_append_distributed.sh`
- `.agent/scripts/kafka_run_append_distributed.sh`
- `.agent/scripts/mofka_run_append_distributed.sh`

## Systems

ChronoLog:

- build and modify ChronoLog only,
- run append, catch-up, and archive/range workflows,
- collect metrics, logs, and profile evidence,
- write common `metrics.json` output.

Kafka:

- launch as a fixed baseline,
- run producer and consumer workflows,
- collect throughput and latency,
- do not modify or tune Kafka source or internals.

Mofka:

- launch as a fixed baseline,
- run memory and PMDK workflow variants,
- collect throughput and latency,
- do not modify or tune Mofka source or internals.

## Result schema

Each system run should produce a comparable `metrics.json` with at least:

```json
{
  "system": "chronolog|kafka|mofka",
  "workflow": "name",
  "node_count": 0,
  "client_count": 0,
  "message_size_bytes": 0,
  "operation_count": 0,
  "duration_seconds": 0,
  "throughput_ops_per_sec": 0,
  "avg_latency_ms": null,
  "p50_latency_ms": null,
  "p95_latency_ms": null,
  "p99_latency_ms": null,
  "success": true
}
```

## Practical reproduction sequence

Use the experiment branch for raw reproduction, then regenerate the reports:

```bash
git switch opt/phase0-profileforge
profileforge/targets/chronolog/build.sh
python3 .agent/scripts/phase0_benchmark_matrix.py --help
python3 .agent/scripts/phase0_six_way_manifest.py --help
python3 .agent/scripts/phase0_evolution_report.py --help
```

The exact run commands depend on the cluster allocation, node count, payload size, and selected workflow. The report should be read together with the manifest and artifact index for the specific row being reproduced.
