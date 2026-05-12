# ChronoLog gperftools CPU Profile Validation

Checkpoint: verify gperftools CPU profile output on ChronoLog run
Status: complete
Time: 2026-05-11 23:21 CT

## Command Summary

The baseline ChronoLog local smoke deployment was started from `.agent/install-consistent/chronolog`. The `chrono-bench` client was run with:

```text
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libprofiler.so
CPUPROFILE=.agent/results/20260511-232017/chronolog/profiles/gperftools/chrono-bench.cpu.prof
CPUPROFILE_FREQUENCY=100
mpirun -n 1 chrono-bench -c .agent/install-consistent/chronolog/conf/chrono-client-conf.json -w -h 1 -t 1 -a 32 -s 64 -b 128 -n 20 -p
```

OpenMPI/gperftools wrote process-suffixed CPU profile files.

## Evidence

- `chrono-bench` exited with status 0.
- Metrics file: `.agent/results/20260511-232017/chronolog/metrics.json`.
- Non-empty CPU profiles:
  - `.agent/results/20260511-232017/chronolog/profiles/gperftools/chrono-bench.cpu.prof_86295`
  - `.agent/results/20260511-232017/chronolog/profiles/gperftools/chrono-bench.cpu.prof_86301`
- `google-pprof --text` successfully parsed both profiles:
  - `.agent/results/20260511-232017/chronolog/profiles/gperftools/chrono-bench.cpu.prof_86295.pprof.txt`
  - `.agent/results/20260511-232017/chronolog/profiles/gperftools/chrono-bench.cpu.prof_86301.pprof.txt`
- Profile stderr includes gperftools summaries: `PROFILE: interrupts/evictions/bytes = 15/0/1176` and `PROFILE: interrupts/evictions/bytes = 4/0/504`.
