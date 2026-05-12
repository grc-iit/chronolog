# ChronoLog Baseline Build

Checkpoint: build ChronoLog baseline
Status: complete
Time: 2026-05-11 22:25 CT

## Build

- Source branch: `opt/phase0-bootstrap`
- Build type: `Release`
- Build directory: `.agent/build-consistent/Release`
- Install directory: `.agent/install-consistent/chronolog`
- Profiling-friendly flags: `-O2 -g -fno-omit-frame-pointer`
- Test setting: `-DCHRONOLOG_BUILD_TESTING=OFF`

## Module Stack

The successful build used the consistent `/mnt/repo` module stack:

```text
gcc/11.4.0/cmake/3.30.5-pq5ntgo
gcc/11.4.0/argobots/1.1-indjy5q
gcc/11.4.0/libfabric/1.22.0-lyviqpl
gcc/11.4.0/mercury/2.3.1-kcmn6u3
gcc/11.4.0/mochi-margo/0.17.0-llh62si
gcc/11.4.0/mochi-thallium/0.10.1-2pari3f
gcc/11.4.0/boost/1.86.0-cs4onpk
gcc/11.4.0/spdlog/1.14.1-y4quhau
gcc/11.4.0/googletest/1.12.1-al2evss
gcc/11.4.0/json-c/0.16-2ofsb6t
gcc/11.4.0/pkgconf/2.2.0-3sczowe
gcc/11.4.0/python/3.11.9-zg4555e
gcc/11.4.0/py-pybind11/2.13.5-pngixx5
openmpi/5.0.5-x2kvx5l/gcc/11.4.0/hdf5/1.14.5-5zbyeqh
```

The HDF5 module above was required because it provides C++ support and `h5c++`.

## Build Fix

Release configure initially failed because `CMakeLists.txt` unconditionally added `test/performance`, but that directory does not contain a `CMakeLists.txt`. The baseline build fix now skips that subdirectory when its CMake file is absent.

## Installed Binaries

```text
.agent/install-consistent/chronolog/bin/chrono-grapher
.agent/install-consistent/chronolog/bin/chrono-keeper
.agent/install-consistent/chronolog/bin/chrono-player
.agent/install-consistent/chronolog/bin/chrono-visor
.agent/install-consistent/chronolog/tools/benchmark/chrono-bench
.agent/install-consistent/chronolog/tools/cli/chrono-client-cli
```

## Evidence

- Configure/build/install log: `.agent/results/20260511-221855/chronolog/stdout.log`
- Configure/build stderr and warnings: `.agent/results/20260511-221855/chronolog/stderr.log`
