# ChronoLog Minimal Local Smoke Test

Status: complete
Time: 2026-05-11 22:28-22:29 CT

## Workflow

- System: ChronoLog
- Workflow: `minimal_local_write_smoke`
- Deployment: local single-user stack
- Services: one ChronoVisor, one ChronoKeeper, one ChronoGrapher, one ChronoPlayer
- Client: `mpirun -n 1 chrono-bench`
- Workload: one chronicle, one story, five write events, 32-128 byte payload range

## Evidence

- Benchmark stdout: `.agent/results/20260511-222650/chronolog/stdout.log`
- Benchmark stderr: `.agent/results/20260511-222650/chronolog/stderr.log`
- Service logs: `.agent/results/20260511-222650/chronolog/logs/`
- Storage output: `.agent/results/20260511-222650/chronolog/output/chronicle_0_0.story_0_0.1778556480.vlen.h5`
- Metrics: `.agent/results/20260511-222650/chronolog/metrics.json`

## Result

ChronoLog started successfully, accepted a five-event write workload from `chrono-bench`, flushed the story through ChronoKeeper and ChronoGrapher, and wrote an HDF5 story chunk. ChronoLog services then stopped cleanly.
