# ChronoLog Minimal Local Smoke Test

Checkpoint: run ChronoLog minimal local smoke test
Status: complete
Time: 2026-05-11 22:29 CT

## Command Summary

The smoke test used the installed baseline tree at `.agent/install-consistent/chronolog` with the same module stack used for the baseline build.

The local deployment was started with:

```text
deploy_local.sh --start --work-dir .agent/install-consistent/chronolog --monitor-dir .agent/results/20260511-222650/chronolog/logs --output-dir .agent/results/20260511-222650/chronolog/output --keepers 1 --record-groups 1
```

The client workload was:

```text
mpirun -n 1 chrono-bench -c .agent/install-consistent/chronolog/conf/chrono-client-conf.json -w -h 1 -t 1 -a 32 -s 64 -b 128 -n 5 -p
```

## Evidence

- ChronoLog process check found `chrono-visor`, `chrono-grapher`, `chrono-player`, and `chrono-keeper` running before the benchmark.
- `chrono-bench` exited with status 0.
- `chrono-client.log` shows successful connect, chronicle create, story acquire, release, destroy, and disconnect.
- `chrono-keeper-1.log` shows `eventCount 5` entering the Keeper story pipeline and RDMA transfer to the Grapher.
- `chrono-grapher-1.log` shows `eventCount 5` and an HDF5 story chunk written.
- Output file: `.agent/results/20260511-222650/chronolog/output/chronicle_0_0.story_0_0.1778556480.vlen.h5`
- Metrics file: `.agent/results/20260511-222650/chronolog/metrics.json`

## Note

Runtime execution requires the module-provided dependency libraries. Running installed binaries without the module stack failed with `libmargo.so.0` missing; the smoke test therefore runs inside the validated module environment.
