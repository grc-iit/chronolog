# ChronoLog Darshan Validation

Result: true with limitation

Darshan dynamic preload was attempted against the ChronoLog local smoke workload. It produced a parseable Darshan log for the MPI `chrono-bench` client process with POSIX, STDIO, and HEATMAP modules.

Evidence:

- stdout: `.agent/results/20260511-232950/chronolog/stdout.log`
- stderr: `.agent/results/20260511-232950/chronolog/stderr.log`
- Darshan log list: `.agent/results/20260511-232950/chronolog/profiles/darshan/darshan-files.txt`
- parsed output: `.agent/results/20260511-232950/chronolog/profiles/darshan/jcernuda_chrono-bench_id107253-107253_5-11-84599-5984536678313972396_1.darshan.parser.txt`
- metrics: `.agent/results/20260511-232950/chronolog/metrics.json`

Limitation: the local daemon-style service launch did not produce a Darshan record for the Grapher HDF5 output path.
