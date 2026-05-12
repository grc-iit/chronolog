# perf Detection and Install

Status: installed with runtime permission limitation.

Evidence directory: `.agent/results/20260511-221033/`

## Detection

`perf` was not present on `PATH`, no module match was available, and no filesystem candidate was found under common system prefixes. The kernel setting is:

```text
/proc/sys/kernel/perf_event_paranoid = 4
```

This blocks unprivileged performance monitoring without `CAP_PERFMON`, `CAP_SYS_PTRACE`, or `CAP_SYS_ADMIN`.

## No-Sudo Install Path

The matching Ubuntu kernel tools packages were downloaded without sudo using `apt-get download` and extracted under:

```text
opt/perf/extract
```

Downloaded packages:

- `linux-tools-5.15.0-176-generic`
- `linux-tools-5.15.0-176`
- `linux-tools-common`

The directly runnable local binary is:

```text
opt/perf/extract/usr/lib/linux-tools-5.15.0-176/perf
```

Environment snippet for later runs:

```text
.agent/config/perf-env.sh
```

## Validation

Version validation succeeded:

```text
perf version 5.15.199
```

Runtime profiling validation failed as expected under current kernel policy:

```text
perf stat -e task-clock -- true
```

Failure reason: `perf_event_paranoid` is `4`. This is not fixable without administrator action or additional Linux capabilities. The later `Verify perf output on ChronoLog run` checkpoint must either run in an allocation/environment where perf events are permitted or document this as a profiling-output blocker.
