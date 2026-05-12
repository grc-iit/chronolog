# gperftools Detection

Status: complete.

Evidence directory: `.agent/results/20260511-221259/`

## Detected Installation

gperftools is installed system-wide through Ubuntu packages:

- `google-perftools` 2.9.1-0ubuntu3
- `libgoogle-perftools-dev` 2.9.1-0ubuntu3
- `libgoogle-perftools4` 2.9.1-0ubuntu3

Detected commands and artifacts:

| Artifact | Path |
|---|---|
| `google-pprof` | `/usr/bin/google-pprof` |
| `pprof-symbolize` | `/usr/bin/pprof-symbolize` |
| CPU profiler header | `/usr/include/gperftools/profiler.h` |
| Heap profiler header | `/usr/include/gperftools/heap-profiler.h` |
| CPU profiler library | `/usr/lib/x86_64-linux-gnu/libprofiler.so` |
| tcmalloc library | `/usr/lib/x86_64-linux-gnu/libtcmalloc.so` |

No module was required. Spack recognizes a `gperftools` package name but no installed Spack package was found.

## Validation

Compiled and ran a C++ probe that calls:

- `ProfilerStart`
- `ProfilerStop`
- `HeapProfilerStart`
- `HeapProfilerDump`
- `HeapProfilerStop`

Validation outputs:

| Output | Path |
|---|---|
| Probe source | `.agent/results/20260511-221259/gperftools/gperftools_probe.cpp` |
| Probe binary | `.agent/results/20260511-221259/gperftools/gperftools_probe` |
| CPU profile | `.agent/results/20260511-221259/gperftools/cpu.prof` |
| Heap profile | `.agent/results/20260511-221259/gperftools/heap-env.prof.0001.heap` |

`google-pprof --text` successfully read `cpu.prof`.
