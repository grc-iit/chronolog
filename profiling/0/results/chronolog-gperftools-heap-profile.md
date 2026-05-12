# ChronoLog gperftools Heap Profile Validation

Checkpoint: verify gperftools heap/allocation profile output on ChronoLog run
Status: complete
Time: 2026-05-11 23:24 CT

## Command Summary

The baseline ChronoLog local smoke deployment was started from `.agent/install-consistent/chronolog`. The `chrono-bench` client was run with:

```text
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc.so
HEAPPROFILE=.agent/results/20260511-232306/chronolog/profiles/gperftools/chrono-bench.heap
HEAP_PROFILE_ALLOCATION_INTERVAL=1048576
mpirun -n 1 chrono-bench -c .agent/install-consistent/chronolog/conf/chrono-client-conf.json -w -h 1 -t 1 -a 32 -s 64 -b 128 -n 20 -p
```

## Evidence

- `chrono-bench` exited with status 0.
- Metrics file: `.agent/results/20260511-232306/chronolog/metrics.json`.
- gperftools emitted 51 non-empty `.heap` files under `.agent/results/20260511-232306/chronolog/profiles/gperftools/`.
- `google-pprof --text` produced parsed text output for 37 heap dumps before post-processing was stopped after enough evidence was collected.
- Representative raw heap profile: `.agent/results/20260511-232306/chronolog/profiles/gperftools/chrono-bench.heap_91088.0010.heap`.
- Representative parsed profile: `.agent/results/20260511-232306/chronolog/profiles/gperftools/chrono-bench.heap_91088.0010.heap.pprof.txt`.
- Stderr shows heap dump messages such as `Dumping heap profile to ... (95 MB allocated cumulatively, 82 MB currently in use)`.

## Note

The full generated heap profile set is preserved in the local result directory. The commit includes representative heap/pprof artifacts and the profile listing rather than every intermediate heap dump.
