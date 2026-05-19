# ChronoLog Phase 0 Measurement Report

This folder is a shareable engineering report for the Phase 0 ChronoLog measurement and optimization-readiness work.

Phase 0 was not intended to declare a winner against Kafka or Mofka. Its purpose was to make the full measurement machine work across ChronoLog, Kafka, and Mofka, while keeping Kafka and Mofka as fixed baselines and making ChronoLog the only system modified, instrumented, and profiled.

## How to read this report

- [01-executive-summary.md](01-executive-summary.md): short status, headline results, and caveats.
- [02-benchmark-semantics.md](02-benchmark-semantics.md): what each benchmark row means and why some rows are not directly comparable.
- [03-results-summary.md](03-results-summary.md): headline throughput tables for append, catch-up, and archive/range workflows.
- [04-chronolog-changes.md](04-chronolog-changes.md): ChronoLog work accepted, rejected, or left default-off.
- [05-deployment-and-reproduction.md](05-deployment-and-reproduction.md): how the benchmark/deployment pipeline is structured and how to reproduce the work.
- [06-validation-and-completion-audit.md](06-validation-and-completion-audit.md): completion criteria, coverage, and audit status.
- [data/artifact-index.md](data/artifact-index.md): original local result paths and generated artifacts.

## Scope boundary

ChronoLog was the target system. It was built, modified, instrumented, profiled, and rerun.

Kafka and Mofka were fixed baselines. They were launched, benchmarked, and measured, but their source code and internals were not modified or tuned.

## Raw artifact note

This branch intentionally contains the shareable report, not the full raw `.agent/results` tree. The raw experiment branch contains many generated run artifacts, logs, profiles, and intermediate outputs. The report keeps the material needed for review in a stable GitHub-visible location without pushing the full experiment history.
