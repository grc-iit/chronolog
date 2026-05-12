# ChronoLog Darshan Validation

Checkpoint: verify Darshan output if applicable
Status: complete with limitation
Time: 2026-05-11 23:30 CT

## Command Summary

The baseline ChronoLog local validation deployment was run with Darshan dynamic instrumentation preloaded:

```text
LD_PRELOAD=/mnt/common/jcernudagarcia/spack/opt/spack/linux-ubuntu22.04-skylake_avx512/gcc-11.4.0/darshan-runtime-3.4.6-u7vfz6edqy4hzj6fnpx6g7inobzd32os/lib/libdarshan.so
DARSHAN_LOG_DIR_PATH=.agent/results/20260511-232950/chronolog/profiles/darshan/logs
mpirun -n 1 chrono-bench -c .agent/install-consistent/chronolog/conf/chrono-client-conf.json -w -h 1 -t 1 -a 32 -s 64 -b 128 -n 5 -p
```

## Evidence

- `chrono-bench` exited with status 0.
- Darshan wrote `.agent/results/20260511-232950/chronolog/profiles/darshan/logs/jcernuda_chrono-bench_id107253-107253_5-11-84599-5984536678313972396_1.darshan`.
- `darshan-parser` produced `.agent/results/20260511-232950/chronolog/profiles/darshan/jcernuda_chrono-bench_id107253-107253_5-11-84599-5984536678313972396_1.darshan.parser.txt`.
- Parsed modules include POSIX, STDIO, and HEATMAP.
- Parsed POSIX records include `.agent/results/20260511-232950/chronolog/logs/chrono-client.log` on `/mnt/common` xfs.
- Metrics file: `.agent/results/20260511-232950/chronolog/metrics.json`.

## Limitation

This local deployment validates Darshan instrumentation for the MPI `chrono-bench` client process. It does not capture the ChronoGrapher HDF5 output file in this mode. The service processes are launched as background daemons by `deploy_local.sh` and then stopped by signal, so they are not a clean MPI job finalization path for Darshan logs. A service-level Darshan capture should be revisited when the benchmark harness launches ChronoLog services under a Darshan-compatible job wrapper.
