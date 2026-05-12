# ProfileForge Bootstrap for ChronoLog

This directory is the first repo-local ProfileForge contract for the ChronoLog case study.

ProfileForge is the optimization framework. ChronoLog is the target system. Kafka and Mofka are fixed baselines.

Phase 0 produced the measurement ingredients under `.agent/` and `profiling/`. This directory makes those ingredients explicit enough for the first optimization iterations:

- target manifests
- edit boundaries
- benchmark and profiler entry points
- evidence expectations
- accept/reject policy
- iteration handoff plan

The current state is ready for controlled iteration setup, not yet a fully autonomous optimization loop. Iteration 1 can be run by following `controller/iteration-1-runbook.md`; the next framework step is to turn that runbook into a controller script.

## Current Readiness

Ready:

- Distributed bare-metal SLURM deployment scripts exist for ChronoLog, Kafka, and Mofka.
- ChronoLog has TAU semantic instrumentation and distributed TAU, perf, gperftools, Darshan, and Linux network measurement outputs.
- Kafka and Mofka have fixed baseline run paths and common `metrics.json` output.
- Top-level `/profiling` has an explicit iteration history with iteration `0` mapped to one result directory.
- `record_groups` is a tunable ChronoLog deployment parameter and has been validated with `record_groups=2`.

Still needed before full autonomy:

- A controller script that reads the manifests and executes build, deploy, benchmark, profile, normalize, diagnose, patch, validate, judge, and commit/rollback.
- Normalized evidence JSON that merges benchmark metrics, TAU, perf, Darshan, network, and application counters into one agent-facing file per run.
- Stronger correctness validators for no lost records, no duplicate records, ordering, and range retrieval.
- A repeated-run statistical acceptance policy rather than one-run decisions.
- A production-scale benchmark matrix with stable operation counts, trials, client counts, and node counts.

## Main Files

- `targets/chronolog/target.yaml`: target manifest for the optimized system.
- `targets/chronolog/allowed_edits.yaml`: explicit optimization scope.
- `targets/kafka/target.yaml`: fixed Kafka baseline contract.
- `targets/mofka/target.yaml`: fixed Mofka baseline contract.
- `controller/iteration-1-runbook.md`: practical first iteration runbook.
- `controller/acceptance-policy.yaml`: initial accept/reject policy.
- `controller/normalize_evidence.py`: creates one agent-facing evidence JSON file for a registered iteration.
- `agents/bottleneck-diagnosis.md`: diagnosis agent input/output contract.
- `agents/patch-agent.md`: patch/tuning agent guardrails.
- `validators/correctness-policy.md`: correctness requirements that must become executable validators.

Generate iteration-0 normalized evidence:

```bash
python3 profileforge/controller/normalize_evidence.py --iteration 0
```

Output:

```text
profileforge/results/0/evidence.json
```
