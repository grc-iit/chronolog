# ChronoLog TAU-Instrumented Build

Checkpoint: build ChronoLog with TAU instrumentation
Status: complete
Time: 2026-05-11 22:47 CT

## Build

- Build directory: `.agent/build-tau/Release`
- Install directory: `.agent/install-tau/chronolog`
- TAU prefix: `opt/tau-2.34`
- CMake option: `CHRONOLOG_ENABLE_TAU_PROFILING=ON`

## Validation

- Configured, rebuilt, and installed the TAU-enabled Release build.
- Verified core installed binaries exist.
- Verified installed `chrono-visor` resolves `libTAUsh-pthread.so` from the local TAU prefix.
- Recorded profiling macro call sites.

Evidence:

- `.agent/results/20260511-224620/chronolog/stdout.log`
- `.agent/results/20260511-224620/chronolog/stderr.log`
- `.agent/results/20260511-224620/chronolog/chrono-visor.tau.ldd`
- `.agent/results/20260511-224620/chronolog/profile-macro-sites.txt`
