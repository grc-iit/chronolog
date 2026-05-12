# ChronoLog gperftools Heap Profile Validation

Result: true

The ChronoLog local smoke workload ran with the `chrono-bench` client preloaded with `libtcmalloc.so` and `HEAPPROFILE` set under the result directory. The benchmark exited successfully and gperftools emitted raw heap dumps plus parsed `google-pprof --text` reports.

Evidence:

- stdout: `.agent/results/20260511-232306/chronolog/stdout.log`
- stderr: `.agent/results/20260511-232306/chronolog/stderr.log`
- profile listing: `.agent/results/20260511-232306/chronolog/profiles/gperftools/profile-files.txt`
- representative heap profile: `.agent/results/20260511-232306/chronolog/profiles/gperftools/chrono-bench.heap_91088.0010.heap`
- representative pprof text: `.agent/results/20260511-232306/chronolog/profiles/gperftools/chrono-bench.heap_91088.0010.heap.pprof.txt`
- metrics: `.agent/results/20260511-232306/chronolog/metrics.json`
