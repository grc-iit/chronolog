# ChronoLog perf Validation

Checkpoint: verify perf output on ChronoLog run
Status: complete with limitation
Time: 2026-05-11 23:18 CT

## Command Summary

The local perf binary from `.agent/config/perf-env.sh` was invoked against the installed ChronoLog `chrono-visor` binary:

```text
perf stat -e task-clock,cycles,instructions,cache-misses -o .agent/results/20260511-231753/chronolog/profiles/perf/perf-stat.txt -- .agent/install-consistent/chronolog/bin/chrono-visor --help
perf record -F 99 -g -o .agent/results/20260511-231753/chronolog/profiles/perf/perf.data -- .agent/install-consistent/chronolog/bin/chrono-visor --help
```

## Evidence

- `/proc/sys/kernel/perf_event_paranoid` was `4`.
- `perf stat` exited `255`.
- `perf record` exited `255`.
- Stderr states that access to performance monitoring and observability operations is limited without `CAP_PERFMON`, `CAP_SYS_PTRACE`, or `CAP_SYS_ADMIN`.
- Evidence directory: `.agent/results/20260511-231753/`.
- `perf-stat.txt` contains only the start marker.
- `perf.data` was created with size `0`, so no usable perf samples were collected.

## Limitation

The perf tool is installed and runnable, but the current kernel policy blocks unprivileged perf event collection. Producing actual perf CPU profiles, flamegraph input, hardware counters, cache behavior, branch behavior, or call-stack hot spots in this environment requires administrator action or a different allocation with the needed Linux capability.
