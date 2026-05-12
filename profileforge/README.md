# ProfileForge Bootstrap for ChronoLog

This directory is the first repo-local ProfileForge contract for the ChronoLog case study.

ProfileForge is the optimization framework. ChronoLog is the target system. Kafka and Mofka are fixed baselines.

Phase 0 produced the measurement ingredients under `.agent/` and `profiling/`. This directory makes those ingredients explicit enough for the first optimization iterations:

- target manifests
- edit boundaries
- benchmark and profiler entry points
- evidence expectations
- executable validation
- repeated-run performance judging
- detached controller launch

The current state is ready for controlled unattended benchmark/profile/validate/judge iterations. The controller does not itself invent source patches; it can run an optional `--patch-command`, and the LLM optimization agent should use the normalized evidence and edit policy to create one bounded change per iteration.

## Current Readiness

Ready:

- Distributed bare-metal SLURM deployment scripts exist for ChronoLog, Kafka, and Mofka.
- ChronoLog has TAU semantic instrumentation and distributed TAU, perf, gperftools, Darshan, and Linux network measurement outputs.
- Kafka and Mofka have fixed baseline run paths and common `metrics.json` output.
- Top-level `/profiling` has an explicit iteration history with iteration `0` mapped to one result directory.
- `record_groups` is a tunable ChronoLog deployment parameter and has been validated with `record_groups=2`.
- `run_loop.py` can execute benchmark matrix runs, correctness validation, evidence normalization, and performance judging.
- `run_profileforge_tmux.sh` can start the loop detached under tmux.

Still needed for stronger autonomy:

- Automatic LLM patch invocation and rollback integration around `--patch-command`.
- Stronger semantic validators that inspect event IDs/order directly instead of relying on current benchmark metrics and artifacts.
- eBPF-based tools after admin enablement.
- A production-scale benchmark matrix with stable operation counts, trials, client counts, and node counts.

## Main Files

- `targets/chronolog/target.yaml`: target manifest for the optimized system.
- `targets/chronolog/allowed_edits.yaml`: explicit optimization scope.
- `targets/kafka/target.yaml`: fixed Kafka baseline contract.
- `targets/mofka/target.yaml`: fixed Mofka baseline contract.
- `controller/iteration-1-runbook.md`: practical first iteration runbook.
- `controller/acceptance-policy.yaml`: initial accept/reject policy.
- `controller/normalize_evidence.py`: creates one agent-facing evidence JSON file for a registered iteration.
- `controller/run_loop.py`: runs benchmark, validation, evidence normalization, and performance judging for controlled iterations.
- `controller/run_profileforge_tmux.sh`: starts the loop detached in tmux.
- `controller/performance_judge.py`: repeated-run accept/reject/rerun decision.
- `agents/bottleneck-diagnosis.md`: diagnosis agent input/output contract.
- `agents/patch-agent.md`: patch/tuning agent guardrails.
- `validators/correctness-policy.md`: correctness requirements that must become executable validators.
- `validators/validate_correctness.py`: executable correctness validator for current metrics/artifacts.

Generate iteration-0 normalized evidence:

```bash
python3 profileforge/controller/normalize_evidence.py --iteration 0
```

Output:

```text
profileforge/results/0/evidence.json
```

Run a dry controller pass:

```bash
python3 profileforge/controller/run_loop.py --dry-run --iterations 1
```

Start a detached tmux run:

```bash
PROFILEFORGE_NODELIST='ares-comp-[03-06]' \
PROFILEFORGE_NODE_COUNTS=2 \
PROFILEFORGE_OPERATION_COUNTS=1000 \
PROFILEFORGE_TRIALS=3 \
PROFILEFORGE_PROFILE_MODE=tau \
bash profileforge/controller/run_profileforge_tmux.sh
```

The tmux launcher does not spend account funds by itself; it only starts the local controller process. Funding/account changes must be handled outside the repository tooling.
