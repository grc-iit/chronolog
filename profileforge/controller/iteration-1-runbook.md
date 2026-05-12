# Iteration 1 Runbook

This is the first controlled ProfileForge iteration after Phase 0.

The purpose is not to make a speculative optimization. The purpose is to run the loop once with enough structure that the next agent can diagnose one bottleneck, make one bounded change, and accept or reject it.

## Preflight

1. Confirm branch and base:

```bash
git branch --show-current
git merge-base --is-ancestor origin/develop HEAD
```

2. Confirm target nodes:

```text
ares-comp-03
ares-comp-04
ares-comp-05
ares-comp-06
```

3. Confirm admin-enabled profiling status:

- `perf_event_paranoid=1` on target nodes.
- `kernel.yama.ptrace_scope` relaxed on target nodes if Mercury shared-memory RMA or local tracing needs it.
- eBPF-based tools still require the admin path in `profiling/0/ebpf-admin-command-allowlist.md`.

## Baseline Inputs

Iteration 0 is:

```text
.agent/results/20260512-122315-chronolog-tau-full-semantics
```

The registered map is:

```text
profiling/data/history/iteration_map.csv
```

Fixed baselines are:

```text
profiling/data/history/fixed_baselines.csv
```

## Recommended First Loop Workload

Start with append throughput because it has the most complete comparable pipeline:

```bash
python3 .agent/scripts/phase0_benchmark_matrix.py \
  --systems chronolog \
  --workflows append_throughput \
  --node-counts 2,4 \
  --message-sizes 1024 \
  --operation-counts 100,1000 \
  --trials 3 \
  --partition debug \
  --nodelist 'ares-comp-[03-06]' \
  --chronolog-profile-mode tau \
  --slurm-time 00:10:00
```

Use larger operation counts after the deployment timing overhead is separated from steady-state throughput. Do not use small operation counts for performance claims.

## Evidence Bundle Required Per Candidate

Each iteration result should contain:

- `metrics.json`
- ChronoLog config manifest
- service logs
- TAU semantic profiles
- perf report or documented perf unavailability
- gperftools CPU/heap profile when enabled
- Darshan output for I/O-focused runs
- Linux network measurement command outputs
- normalized evidence JSON
- correctness result
- diagnosis
- proposed change
- accept/reject decision

## First Useful Implementation Gap

Before allowing code optimization, implement an evidence normalizer that writes:

```text
profileforge/results/<iteration>/evidence.json
```

That file should merge:

- common benchmark metrics
- TAU top semantic regions
- perf top symbols and lock/futex indicators
- Darshan I/O summary
- Linux network counters
- application counters when available
- links to raw evidence

The optimization agent should consume this normalized evidence rather than raw logs alone.

The current bootstrap normalizer is:

```bash
python3 profileforge/controller/normalize_evidence.py --iteration 0
```

## Candidate First Bottleneck Areas

The next agent should not assume Keeper locks are the only bottleneck. It should compare:

- client append/RPC time
- Visor metadata and recording-start path
- Keeper ingest, lock wait, lock hold, buffer insert, and flush
- Grapher collection and archive path
- Player/replay path for range retrieval
- network and storage measurements

The selected change must be one bounded change from `profileforge/targets/chronolog/allowed_edits.yaml`.

## Completion Criteria

Iteration 1 is complete only when:

- build passes
- deployment passes
- selected benchmark runs
- profiler evidence is collected or a blocker is recorded
- correctness checks pass
- performance is compared to iteration 0 and fixed baselines
- accept/reject decision is written
- `profiling/data/history/iteration_map.csv` is updated only if the iteration is deliberately recorded
- `python3 profiling/scripts/generate_loop_history.py` is rerun
