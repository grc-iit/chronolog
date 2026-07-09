# chrono-ldms benchmarks

Standalone benchmarks used to characterize the ChronoLog storage backend for
LDMS (the [`plugins/chrono-ldms`](../../../plugins/chrono-ldms) `store_chronolog`
plugin) and to compare it against Kafka under matching, LDMS-shaped workloads.

> These are **not** gtest unit tests and are intentionally **not** wired into
> the CMake / `ctest` build: they depend on external services (a running
> ChronoLog deployment, a Kafka broker) and libraries (`librdkafka`, the
> ChronoLog client, optionally LDMS). Build and run them by hand with the
> scripts here.

## Contents

| file | what it is |
| --- | --- |
| `bench_common.h` | shared LDMS-shaped JSON payload generator + timing helpers |
| `chrono_bench.cpp` | ChronoLog write throughput (many-writers, one story per thread) |
| `kafka_bench.cpp` | Kafka write throughput (batched and per-event `KAFKA_SYNC` modes) |
| `chrono_read_bench.cpp` | ChronoLog read: tail read via `playback()` (writer-only client) |
| `kafka_read_bench.cpp` | Kafka read (tail/consume) |
| `tail_test.cpp` | functional probe of the keeper-side tail read (`playback()`) |
| `ingest_bench.cpp` | local micro-benchmark of the ingest data-structure delta |
| `chrono_rw_test.cpp` | round-trip write+read smoke test |
| `build.sh` | builds `kafka_bench` + `chrono_bench` against installed deps |
| `run_*.sh` | workload drivers (matrix / final / read / concurrency) |
| `*.csv` | recorded result data |
| `REPORT.md`, `WORKLOADS.md`, `LDMS-ARCHITECTURE.md`, `LIVE-TAIL-DESIGN.md` | writeups |

## Building

```bash
# needs a ChronoLog install + librdkafka (pkg-config rdkafka)
CHRONO_PREFIX=$HOME/chronolog-install/chronolog ./build.sh
```

`build.sh` compiles in place and uses absolute install paths, so it works from
this location unchanged. The read/tail benchmarks require the keeper-side tail
read (`StoryHandle::playback()`), which is why this work lives on the tail-read
development line.

## Running

Deploy ChronoLog (see the repo `CLAUDE.md`) and start a Kafka broker, then use
the `run_*.sh` drivers. See `REPORT.md` for the methodology and results, and
`LDMS-ARCHITECTURE.md` for how LDMS aggregation maps onto ChronoLog.
