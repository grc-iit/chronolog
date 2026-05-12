# Phase 0 Profiling Package

This folder contains the showable Phase 0 baseline profiling package for ChronoLog.

Primary distributed evidence:

- TAU distributed run: `.agent/results/20260512-094406`
- gperftools distributed run: `.agent/results/20260512-094726`
- Darshan distributed run: `.agent/results/20260512-095335`
- Mofka Yokan/Warabi-backed default partition runs: `.agent/results/20260512-091538` and `.agent/results/20260512-093629`

Figures:

- `figures/profiler_coverage.png`: which collectors are validated, permission-limited, or planned.
- `figures/tau_semantic_events.png`: semantic ChronoLog TAU user events from the distributed client.
- `figures/gperftools_keeper_cpu_samples.png`: top ChronoKeeper CPU samples from gperftools.

TAU visualization tools found locally:

- `opt/tau-2.34/x86_64/bin/paraprof`
- `opt/tau-2.34/x86_64/bin/pprof`
- `opt/tau-2.34/x86_64/bin/jumpshot`
- `opt/tau-2.34/x86_64/bin/tau2slog2`
- `opt/tau-2.34/x86_64/bin/tau_prof2json.py`

For static repo figures, `pprof` text output plus generated matplotlib charts are the most portable path. For interactive TAU inspection, use ParaProf on the `profile.*` directories. Jumpshot timeline images require a TAU trace-mode run; this package validates TAU profile mode and marks trace-mode timeline generation as next work.
