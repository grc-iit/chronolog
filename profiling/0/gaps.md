# Phase 0 Gap Audit

The audit found four meaningful gaps after the earlier premature completion note.

| Gap | Status | Evidence |
|---|---|---|
| Mofka storage | Fixed | Default partition runs backed by configured Yokan metadata and Warabi data providers: `.agent/results/20260512-091538`, `.agent/results/20260512-093629` |
| Distributed ChronoLog profiling | Improved, still needs perf/eBPF completion | TAU semantic durations `.agent/results/20260512-110654`, gperftools `.agent/results/20260512-094726`, Darshan `.agent/results/20260512-095335` |
| ChronoLog reading | Documented current behavior and future gap | Current comparable path is archive-backed replay after Grapher HDF5 archival; live/tail read remains future ChronoLog work |
| Benchmark framework | Seeded as loop framework requirement | `profiling/0/benchmark-framework.md` maps Mofka generator dimensions to the common harness and loop-agent search space |

Remaining external limitations and newly enabled targets:

- Target nodes for low-level profiling are `ares-comp-03` through `ares-comp-06`.
- `kernel.perf_event_paranoid=1` and `kernel.yama.ptrace_scope=0` are enabled on `ares-comp-03` and `ares-comp-04`; Kun Feng is extending this to `ares-comp-05` and `ares-comp-06`.
- `perf` is still not on `PATH` in the current environment, so the next blocker is tool availability, not kernel policy.
- eBPF-based tools still need an approved wrapper/capability path because `unprivileged_bpf_disabled=2` remains set.

The current TAU semantic timing is useful but not yet complete enough for all optimization decisions. Keeper lock/queue instrumentation still needs finer labels around queue push/pop, lock hold/wait, ingestion-thread scheduling, and batching.
