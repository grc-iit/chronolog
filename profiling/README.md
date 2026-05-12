# ChronoLog Profiling Story

This directory is the cross-iteration profiling and benchmark history for the ChronoLog optimization loop.

Iteration snapshots:

- `0/`: Phase 0 baseline evidence package.

Cross-iteration history:

- `data/history/chronolog_metrics_history.csv`: ChronoLog workflow metrics collected from timestamped `.agent/results` runs.
- `data/history/tau_semantic_history.csv`: TAU semantic-region timing collected from timestamped `.agent/results` runs.
- `figures/chronolog_throughput_over_time.png`: append-throughput evolution across runs.
- `figures/tau_semantic_time_over_time.png`: semantic timing evolution across runs.

Regenerate the history after new loop iterations:

```bash
python3 profiling/scripts/generate_loop_history.py
```

The top-level history is intended to grow over optimization iterations. The `0/` folder should remain the iteration 0 baseline snapshot.
