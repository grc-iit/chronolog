# ChronoLog gperftools CPU Profile Validation

Result: true

The ChronoLog local smoke workload ran with the `chrono-bench` client preloaded with `libprofiler.so` and `CPUPROFILE` set under the result directory. Because `chrono-bench` runs under OpenMPI, gperftools wrote MPI process-suffixed CPU profile files.

Evidence:

- stdout: `.agent/results/20260511-232017/chronolog/stdout.log`
- stderr: `.agent/results/20260511-232017/chronolog/stderr.log`
- CPU profiles: `.agent/results/20260511-232017/chronolog/profiles/gperftools/chrono-bench.cpu.prof_86295`, `.agent/results/20260511-232017/chronolog/profiles/gperftools/chrono-bench.cpu.prof_86301`
- pprof text: `.agent/results/20260511-232017/chronolog/profiles/gperftools/chrono-bench.cpu.prof_86295.pprof.txt`, `.agent/results/20260511-232017/chronolog/profiles/gperftools/chrono-bench.cpu.prof_86301.pprof.txt`
- metrics: `.agent/results/20260511-232017/chronolog/metrics.json`
