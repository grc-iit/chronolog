# Phase 0 Gap Audit

The audit found four meaningful gaps after the earlier premature completion note.

| Gap | Status | Evidence |
|---|---|---|
| Mofka storage | Fixed | Default partition runs backed by configured Yokan metadata and Warabi data providers: `.agent/results/20260512-091538`, `.agent/results/20260512-093629` |
| Distributed ChronoLog profiling | Perf fixed; eBPF still admin-gated | Keeper lock/queue TAU `.agent/results/20260512-115145-chronolog-tau-keeper-semantics`, perf `.agent/results/20260512-115003-chronolog-perf`, gperftools `.agent/results/20260512-094726`, Darshan `.agent/results/20260512-095335` |
| ChronoLog reading | Documented current behavior and future gap | Current comparable path is archive-backed replay after Grapher HDF5 archival; live/tail read remains future ChronoLog work |
| Benchmark framework | Implemented initial tunable runner | `.agent/scripts/phase0_benchmark_matrix.py`; validated run `.agent/results/20260512-115546-benchmark-matrix-chronolog`; figure `profiling/0/figures/benchmark_append_throughput_matrix.png` |

Remaining external limitations and newly enabled targets:

- Target nodes for low-level profiling are `ares-comp-03` through `ares-comp-06`.
- `kernel.perf_event_paranoid=1` and `kernel.yama.ptrace_scope=0` are enabled on `ares-comp-03` and `ares-comp-04`; Kun Feng is extending this to `ares-comp-05` and `ares-comp-06`.
- `perf` is still not on default `PATH`, but the repo-local kernel-matched binary at `opt/perf/bin/perf` was validated on `ares-comp-03` and used by the distributed harness.
- eBPF-based tools still need an approved wrapper/capability path because `unprivileged_bpf_disabled=2` remains set.

The current TAU semantic timing now includes Keeper lock/queue labels. It is ready for larger contention-oriented runs, but queue pop/wait, extraction batch, HDF5 open/write/flush, and TAU trace-mode timelines remain useful next refinements.
