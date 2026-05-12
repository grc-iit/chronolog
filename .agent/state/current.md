# Current State

- current task: strengthen Phase 0 profiling package into actionable optimization-loop input
- commands running: none
- last successful validation: TAU semantic-duration instrumentation rebuilt and validated in distributed run `.agent/results/20260512-110654`; profiling package now includes raw TAU/Darshan/network artifacts, parsed TAU semantic durations, parsed gperftools role samples, parsed Darshan role I/O, and regenerated figures
- current blocker: none active
- open issue: target nodes are `ares-comp-03` through `ares-comp-06`; `perf_event_paranoid=1` and `ptrace_scope=0` are confirmed on comp-03/04, but `perf` binary is not on PATH and eBPF-based tools still need an allowlisted wrapper because `unprivileged_bpf_disabled=2`
- next intended step: implement the richer tunable benchmark framework and add finer Keeper lock/queue instrumentation for the optimization loop
