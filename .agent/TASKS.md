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
- [x] Install or detect Darshan.
- [x] Detect eBPF-based tooling through available bpftrace or BCC tools.
- [x] Detect Linux network measurement commands: iperf3, ss, nstat, sar -n, ethtool.
- [x] Write toolchain report.

## ChronoLog

- [x] Verify current branch is off `develop`.
- [x] Build ChronoLog baseline.
- [x] Run ChronoLog minimal local smoke test.
- [x] Add ChronoLog profiling abstraction.
- [x] Add TAU-backed ChronoLog profiling mode.
- [x] Add no-op profiling mode.
- [x] Add coarse semantic regions.
- [x] Build ChronoLog with TAU instrumentation.
- [x] Run ChronoLog instrumented smoke test.
- [x] Verify TAU output.
- [x] Verify perf output on ChronoLog run.
- [x] Verify gperftools CPU profile output on ChronoLog run.
- [x] Verify gperftools heap/allocation profile output on ChronoLog run.
- [x] Verify Darshan output if applicable.
- [x] Verify eBPF-based measurements if available.
- [x] Verify Linux network measurement outputs.
- [x] Run append throughput workflow against ChronoLog distributed target.

## Kafka fixed baseline

- [x] Create Kafka launch script.
- [x] Create Kafka stop/cleanup script.
- [x] Run selected benchmark/workflow against Kafka.
- [x] Run append throughput workflow against Kafka distributed target.
- [x] Collect Kafka baseline throughput/latency.
- [x] Confirm no Kafka source changes were made.

## Mofka fixed baseline

- [x] Install or expose Mofka fixed-baseline tooling without sudo.
- [x] Explore bundled Mofka benchmark tooling and record whether it can drive Phase 0.
- [x] Create Mofka launch script.
- [x] Create Mofka stop/cleanup script.
- [x] Validate Mofka local smoke launch and Bedrock provider query.
- [x] Validate Mofka two-node bare-metal launch and Bedrock provider query.
- [x] Run selected benchmark/workflow against Mofka.
- [x] Run append throughput workflow against Mofka distributed target.
- [x] Collect Mofka baseline throughput/latency.
- [x] Confirm no Mofka source changes were made.

## Configuration characterization

- [x] Record distributed deployment policy: bare metal SLURM target, containerized fallback, local master-node smoke only.
- [x] Define configuration surfaces that must be captured for ChronoLog, Kafka, and Mofka.
- [x] Capture per-run configuration manifest for ChronoLog.
- [x] Capture per-run configuration manifest for Kafka.
- [x] Capture per-run configuration manifest for Mofka local smoke.
- [x] Record RDMA/RoCE and Linux network measurement evidence for distributed runs.
- [x] Justify selected ChronoLog, Kafka, and Mofka configurations in the final Phase 0 report.

## Common benchmark/result harness

- [x] Create shared workload config format.
- [x] Create common metrics output schema.
- [x] Create distributed SLURM run wrapper.
- [x] Ensure every local smoke run writes `metrics.json`.
- [x] Ensure every local smoke run writes stdout/stderr/logs.
- [x] Ensure ChronoLog local smoke profile artifacts are collected.
- [x] Create final Phase 0 report.

## Phase 0 completion

- [x] Append throughput workflow runs on ChronoLog distributed target.
- [x] Append latency workflow runs on ChronoLog distributed target.
- [x] Range retrieval workflow runs on ChronoLog distributed target.
- [x] All selected workflows run on ChronoLog distributed target.
- [x] Append throughput workflow runs on Kafka distributed target.
- [x] Append latency workflow runs on Kafka distributed target.
- [x] Range retrieval workflow runs on Kafka distributed target.
- [x] All selected workflows run on Kafka distributed target.
- [x] Append throughput workflow runs on Mofka distributed target.
- [x] Append latency workflow runs on Mofka distributed target.
- [x] Range retrieval workflow runs on Mofka distributed target.
- [x] All selected workflows run on Mofka distributed target.
- [x] ChronoLog instrumentation works.
- [x] ChronoLog profiling stack produces outputs.
- [x] Results are comparable across systems.
- [x] Final Phase 0 report exists.

## Gap wrap-up after completion audit correction

- [x] Validate Mofka default partition storage backed by configured Yokan/Warabi providers.
- [x] Run ChronoLog distributed profiling with TAU across client and service roles.
- [x] Run ChronoLog distributed profiling with gperftools CPU/heap across service roles.
- [x] Run ChronoLog distributed profiling with Darshan across client and service roles.
- [x] Document ChronoLog archive-backed read path and future live/tail read gap.
- [x] Seed benchmark framework design from Mofka generator configuration dimensions.
- [x] Package Phase 0 profiling outputs and figures under `profiling/0`.
- [x] Add finer Keeper lock/queue TAU semantic labels and validate them in a distributed TAU run.
- [x] Broaden TAU semantic labels across client, Visor, Keeper, Grapher, Player, RPC, archive, and HDF5 paths.
- [x] Make `perf` available without sudo through a repo-local kernel-matched binary and validate distributed `perf record`.
- [x] Implement and validate the tunable Phase 0 benchmark matrix runner.
- [x] Generate updated TAU, perf, benchmark matrix, and over-time loop-history artifacts under `profiling/0`.
- [x] Move loop history to top-level `/profiling` and keep `profiling/0` as the iteration-0 snapshot.
- [x] Validate `record_groups=2` as a tunable ChronoLog deployment parameter.
- [x] Document ChronoLog bare-metal SLURM service placement for engineer review.
- [x] Add ProfileForge target manifests, edit policy, acceptance policy, and iteration-1 runbook.
- [x] Generate normalized iteration-0 evidence JSON for the optimization-agent handoff.
- [x] Add executable correctness validator, repeated-run performance judge, controller loop, and tmux launcher.
- [x] Add explicit ProfileForge goal mode for `>=2x` ChronoLog-vs-Mofka across selected benchmarks.
- [x] Add nonfatal ChronoLog eBPF profile mode and validate current sudo/tool status on Ares.
