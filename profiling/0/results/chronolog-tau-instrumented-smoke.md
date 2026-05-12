# ChronoLog TAU-Instrumented Smoke Test

Checkpoint: run ChronoLog instrumented smoke test
Status: complete
Time: 2026-05-11 23:15 CT

## Command Summary

The smoke test used the TAU-instrumented install tree at `.agent/install-tau/chronolog`.

The local deployment was started with:

```text
deploy_local.sh --start --work-dir .agent/install-tau/chronolog --monitor-dir .agent/results/20260511-231340/chronolog/logs --output-dir .agent/results/20260511-231340/chronolog/output --keepers 1 --record-groups 1
```

The client workload was:

```text
mpirun -n 1 chrono-bench -c .agent/install-tau/chronolog/conf/chrono-client-conf.json -w -h 1 -t 1 -a 32 -s 64 -b 128 -n 5 -p
```

## Evidence

- `chrono-bench` exited with status 0.
- ChronoLog services stopped cleanly after the run.
- Metrics file: `.agent/results/20260511-231340/chronolog/metrics.json`.
- TAU profile files: `.agent/results/20260511-231340/chronolog/profiles/profile.0.0.0` and `.agent/results/20260511-231340/chronolog/profiles/profile.0.0.1`.
- TAU profile user events include `client_append`, `rpc_send`, `rpc_receive`, `metadata_lookup`, `story_index_update`, and `append_bytes`.

## TAU Runtime Note

TAU pthread timers conflicted with ChronoLog's Thallium/Argobots RPC callback scheduling and produced timer-stack overlap aborts in `chrono-visor`. The ChronoLog profiling abstraction now records `CL_PROFILE_REGION` semantic regions as TAU user events in TAU mode. This preserves source-level semantic TAU output for Phase 0 without making the measurement pipeline depend on pthread timer-stack behavior inside user-level scheduled RPC handlers.
