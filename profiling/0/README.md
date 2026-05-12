# Phase 0 Profiling Package

This folder contains the Phase 0 baseline profiling package for ChronoLog. It is intended to be consumed by the follow-on optimization loop, not just used as proof that tools launched.

Primary distributed evidence:

- TAU distributed run: `.agent/results/20260512-094406`
- gperftools distributed run: `.agent/results/20260512-094726`
- Darshan distributed run: `.agent/results/20260512-095335`
- Mofka Yokan/Warabi-backed default partition runs: `.agent/results/20260512-091538` and `.agent/results/20260512-093629`
- TAU semantic-duration run: `.agent/results/20260512-110654`
- Keeper lock/queue TAU semantic run: `.agent/results/20260512-115145-chronolog-tau-keeper-semantics`
- broad ChronoLog semantic TAU run: `.agent/results/20260512-122315-chronolog-tau-full-semantics`
- distributed perf run on `ares-comp-[03-04]`: `.agent/results/20260512-115003-chronolog-perf`
- tunable benchmark matrix run: `.agent/results/20260512-115546-benchmark-matrix-chronolog`

Target profiling nodes:

- `ares-comp-03`
- `ares-comp-04`
- `ares-comp-05`
- `ares-comp-06`

Kun Feng enabled `kernel.perf_event_paranoid=1` and `kernel.yama.ptrace_scope=0` on `ares-comp-03` and `ares-comp-04`, and is extending the same settings to `ares-comp-05` and `ares-comp-06`. A repo-local kernel-matched `perf` binary was validated on `ares-comp-03`. `unprivileged_bpf_disabled=2` was still present when checked on `ares-comp-03` and `ares-comp-04`, so eBPF-based tools still need an approved wrapper/capability path or temporary profiling-node enablement.

Figures:

- `figures/profiler_coverage.png`: which collectors are validated, permission-limited, or planned.
- `figures/tau_semantic_time_breakdown.png`: observed semantic time by role/region from TAU events.
- `figures/tau_semantic_tail_latency.png`: max observed semantic-region latency by role/region.
- `figures/tau_semantic_events.png`: semantic ChronoLog TAU event counts from the distributed client.
- `figures/gperftools_keeper_cpu_samples.png`: top ChronoKeeper CPU samples from gperftools.
- `figures/gperftools_cpu_by_role.png`: gperftools sample signal by ChronoLog role.
- `figures/darshan_io_by_role.png`: Darshan-observed I/O volume by ChronoLog role.
- `figures/benchmark_append_throughput_matrix.png`: initial tunable benchmark matrix throughput sweep.

Raw and parsed data:

- `raw/tau/`: TAU profiles from the Keeper lock/queue semantic distributed run.
- `raw/perf/`: raw `perf.data`, `perf stat`, and `perf report --stdio` outputs from the distributed perf run.
- `raw/darshan/`: distributed Darshan logs for client and service roles.
- `raw/network/`: Linux network measurement command outputs.
- `data/tau/semantic_region_durations.csv`: parsed semantic timings.
- `data/benchmark/benchmark_matrix_summary.csv`: parsed benchmark matrix metrics.
- `data/gperftools/top_cpu_samples_by_role.csv`: parsed top CPU samples by role.
- `data/darshan/io_summary_by_role.csv`: parsed I/O counters by role.
- `perf-local-install.md`: no-sudo commands used to recreate the repo-local kernel-matched `perf`.

TAU visualization tools found locally:

- `opt/tau-2.34/x86_64/bin/paraprof`
- `opt/tau-2.34/x86_64/bin/pprof`
- `opt/tau-2.34/x86_64/bin/jumpshot`
- `opt/tau-2.34/x86_64/bin/tau2slog2`
- `opt/tau-2.34/x86_64/bin/tau_prof2json.py`

For static repo figures, `pprof` text output plus generated matplotlib charts are the most portable path. For interactive TAU inspection, use ParaProf on the `profile.*` directories. Jumpshot timeline images require a TAU trace-mode run; this package validates TAU profile mode and marks trace-mode timeline generation as next work.

The optimization loop should append new runs under `.agent/results/YYYYMMDD-HHMMSS.../` and rerun `profiling/scripts/generate_loop_history.py`. Cross-iteration CSVs and figures live at top-level `profiling/`; this `0/` folder is the iteration 0 snapshot.
