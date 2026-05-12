# Current State

- current task: add TAU-backed ChronoLog profiling mode
- commands running: none
- last successful validation: ChronoLog profiling abstraction compiled, built, and installed; evidence in `.agent/results/20260511-223210/chronolog/stdout.log` and `.agent/results/chronolog-profiling-abstraction.md`
- current blocker: perf runtime events and eBPF-based observability require cluster/admin permission changes for later profiling-output validation
- next intended step: wire the profiling abstraction to TAU behind an explicit CMake option while keeping default builds unchanged
