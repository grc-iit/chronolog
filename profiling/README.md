# ChronoLog Profiling Story

This directory is the cross-iteration profiling and benchmark history for the ChronoLog optimization loop.

Iteration snapshots:

- `0/`: Phase 0 baseline evidence package.

Cross-iteration history:

- `data/history/iteration_map.csv`: explicit mapping from optimization iteration number to the timestamped result directory. It currently has exactly one entry: iteration `0`.
- `data/history/fixed_baselines.csv`: fixed Kafka and Mofka comparison baselines. These systems are comparison references, not optimization targets.
- `data/history/chronolog_metrics_history.csv`: ChronoLog workflow metrics for registered iterations only, including ChronoLog-to-iteration-0, ChronoLog-to-Kafka, and ChronoLog-to-Mofka throughput ratios.
- `data/history/tau_semantic_history.csv`: TAU semantic-region timing for registered iterations only. Multiple rows can exist for one iteration because each row is one semantic region/role, not a separate optimization point.
- `figures/chronolog_throughput_over_time.png`: append-throughput evolution across runs.
- `figures/chronolog_throughput_ratio_to_baselines.png`: ChronoLog throughput ratio to iteration 0, Kafka, and Mofka for the current registered point.
- `figures/tau_semantic_time_over_time.png`: semantic timing evolution across runs.

Regenerate the history after new loop iterations:

```bash
python3 profiling/scripts/generate_loop_history.py
```

The top-level history is intended to grow over optimization iterations. The `0/` folder should remain the iteration 0 baseline snapshot.

Do not populate the top-level history by scanning every validation result. Add new rows to `iteration_map.csv` only when the optimization loop accepts or records a deliberate iteration.
