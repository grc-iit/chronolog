#ifndef CHRONOLOG_PROFILE_H
#define CHRONOLOG_PROFILE_H

// ChronoLog profiling abstraction.
//
// Profiling call sites should use these macros instead of directly calling a
// profiler API. Disabled profiling must not evaluate macro arguments.

#define CL_PROFILE_REGION(name) ((void)0)
#define CL_PROFILE_COUNTER(name, value) ((void)0)

#endif // CHRONOLOG_PROFILE_H
