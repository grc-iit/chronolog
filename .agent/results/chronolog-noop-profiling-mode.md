# ChronoLog No-Op Profiling Mode

Checkpoint: add no-op profiling mode
Status: complete
Time: 2026-05-11 22:39 CT

## Validation

- Compiled and ran a probe using `CL_PROFILE_REGION` and `CL_PROFILE_COUNTER` without TAU enabled.
- The probe passed only if disabled profiling did not evaluate side-effecting macro arguments.
- Reconfigured, rebuilt, and reinstalled the default Release build with `CHRONOLOG_ENABLE_TAU_PROFILING=OFF`.
- Verified installed `chrono-visor` has no `libTAU` dependency.

Evidence:

- `.agent/results/20260511-223810/chronolog/stdout.log`
- `.agent/results/20260511-223810/chronolog/stderr.log`
- `.agent/results/20260511-223810/chronolog/chrono-visor.noop.ldd`
