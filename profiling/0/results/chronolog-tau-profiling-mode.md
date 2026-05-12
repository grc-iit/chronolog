# ChronoLog TAU Profiling Mode

Checkpoint: add TAU-backed ChronoLog profiling mode
Status: complete
Time: 2026-05-11 22:36 CT

## Changes

- Added `CHRONOLOG_ENABLE_TAU_PROFILING`.
- Added `CHRONOLOG_TAU_ROOT` discovery from `$TAU_PREFIX`.
- TAU-enabled builds define:
  - `CHRONOLOG_PROFILE_TAU=1`
  - `PROFILING_ON=1`
  - `TAU_DOT_H_LESS_HEADERS=1`
- `CL_PROFILE_REGION(name)` maps to a ChronoLog RAII wrapper over TAU timers.
- `CL_PROFILE_COUNTER(name, value)` maps to TAU user events.
- TAU-enabled installs append the TAU library directory to install RPATH.

## Validation

- Compiled and ran a TAU-enabled probe using `chronolog_profile.h`.
- Configured a TAU-enabled Release build in `.agent/build-tau/Release`.
- Built and installed to `.agent/install-tau/chronolog`.
- Verified `chrono-visor` is installed.
- Verified installed `chrono-visor` resolves `libTAUsh-pthread.so` from the local TAU prefix.

Evidence:

- `.agent/results/20260511-223430/chronolog/stdout.log`
- `.agent/results/20260511-223430/chronolog/stderr.log`
- `.agent/results/20260511-223430/chronolog/chrono-visor.tau.ldd`
- `.agent/results/20260511-223430/chronolog/profiles/tau_probe_profile.-1.0.0`

## Note

The first probe attempt documented in stderr failed until `TAU_DOT_H_LESS_HEADERS` was added. The retry succeeded and the final validation passed.
