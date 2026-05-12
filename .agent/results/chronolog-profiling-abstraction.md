# ChronoLog Profiling Abstraction

Checkpoint: add ChronoLog profiling abstraction
Status: complete
Time: 2026-05-11 22:31 CT

## Changes

- Added `Client/cpp/include/chronolog_profile.h`.
- Added public macros:
  - `CL_PROFILE_REGION(name)`
  - `CL_PROFILE_COUNTER(name, value)`
- Added the profiling header to the installed ChronoLog client public headers.

## Current Behavior

The abstraction currently compiles to no-op macros. Disabled profiling does not evaluate macro arguments, so call sites can be added without changing runtime behavior until a profiling backend is enabled.

## Validation

- Compiled a standalone probe including `chronolog_profile.h` with `-Wall -Wextra -Werror`.
- Reconfigured the existing Release build.
- Rebuilt ChronoLog.
- Reinstalled ChronoLog and verified `.agent/install-consistent/chronolog/include/chronolog_profile.h` exists.

Evidence:

- `.agent/results/20260511-223210/chronolog/stdout.log`
- `.agent/results/20260511-223210/chronolog/stderr.log`
