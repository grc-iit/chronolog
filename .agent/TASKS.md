# Phase 0 Task Board - ChronoLog Measurement Pipeline

## Rule

Phase 0 is complete only when the selected workflows/benchmarks run on ChronoLog, Kafka, and Mofka, and ChronoLog produces profiling outputs from the configured profiling stack.

This is not a performance-optimization phase. The goal is to make the full measurement pipeline work.

## Critical first task

- [x] Define the required Phase 0 workflows/benchmarks across ChronoLog, Kafka, and Mofka.

## Environment and tooling

- [x] Detect SLURM environment: partition, node count, job limits, modules, compiler stack.
- [x] Create no-sudo install strategy: modules first, user-local prefix second, source build third.
- [x] Install or detect TAU.
- [x] Install or detect perf.
- [x] Install or detect gperftools.
- [ ] Install or detect Darshan.
- [ ] Detect eBPF-based tooling through available bpftrace or BCC tools.
- [ ] Detect Linux network measurement commands: iperf3, ss, nstat, sar -n, ethtool.
- [ ] Write toolchain report.

## ChronoLog

- [ ] Verify current branch is off `develop`.
- [ ] Build ChronoLog baseline.
- [ ] Run ChronoLog minimal local smoke test.
- [ ] Add ChronoLog profiling abstraction.
- [ ] Add TAU-backed ChronoLog profiling mode.
- [ ] Add no-op profiling mode.
- [ ] Add coarse semantic regions.
- [ ] Build ChronoLog with TAU instrumentation.
- [ ] Run ChronoLog instrumented smoke test.
- [ ] Verify TAU output.
- [ ] Verify perf output on ChronoLog run.
- [ ] Verify gperftools CPU profile output on ChronoLog run.
- [ ] Verify gperftools heap/allocation profile output on ChronoLog run.
- [ ] Verify Darshan output if applicable.
- [ ] Verify eBPF-based measurements if available.
- [ ] Verify Linux network measurement outputs.

## Kafka fixed baseline

- [ ] Create Kafka launch script.
- [ ] Create Kafka stop/cleanup script.
- [ ] Run selected benchmark/workflow against Kafka.
- [ ] Collect Kafka baseline throughput/latency.
- [ ] Confirm no Kafka source changes were made.

## Mofka fixed baseline

- [ ] Create Mofka launch script.
- [ ] Create Mofka stop/cleanup script.
- [ ] Run selected benchmark/workflow against Mofka.
- [ ] Collect Mofka baseline throughput/latency.
- [ ] Confirm no Mofka source changes were made.

## Common benchmark/result harness

- [ ] Create shared workload config format.
- [ ] Create common metrics output schema.
- [ ] Ensure every run writes `metrics.json`.
- [ ] Ensure every run writes stdout/stderr/logs.
- [ ] Ensure ChronoLog profile artifacts are collected.
- [ ] Create final Phase 0 report.

## Phase 0 completion

- [ ] All selected workflows run on ChronoLog.
- [ ] All selected workflows run on Kafka.
- [ ] All selected workflows run on Mofka.
- [ ] ChronoLog instrumentation works.
- [ ] ChronoLog profiling stack produces outputs.
- [ ] Results are comparable across systems.
- [ ] Final Phase 0 report exists.
