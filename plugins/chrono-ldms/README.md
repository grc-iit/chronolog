# chrono-ldms — LDMS store plugin backed by ChronoLog

`store_chronolog` is an LDMS `ldmsd` **store** plugin that writes aggregated
metric-set samples into [ChronoLog](https://github.com/grc-iit/ChronoLog), a
distributed tiered log / time-series store. It is the ChronoLog side of the
LDMS ↔ ChronoLog integration: LDMS aggregators route sampled metric sets to it,
and it appends each sample as a ChronoLog event.

## Data model mapping

| LDMS                              | ChronoLog             |
| --------------------------------- | --------------------- |
| storage policy `container=`       | Chronicle             |
| storage policy `schema=`          | Story                 |
| one metric-set sample             | one Event (JSON line) |

Each `store()` call serializes the selected metrics of the set into a single
JSON object and appends it to the story via `StoryHandle::log_event()`:

```json
{"timestamp":1782530790.001967,"producer":"node1","instance":"node1/meminfo",
 "schema":"meminfo","metrics":{"component_id":1,"MemTotal":65467136, ...}}
```

Scalar metrics become JSON numbers (char → string); array metrics become JSON
arrays (char arrays → string). `LIST` / `RECORD` metric types are emitted as
`null` (not yet supported).

## Architecture

`ldmsd.h` is C-only and the ChronoLog client API is C++-only, so the plugin is
split across two translation units joined by a small C ABI:

- `src/store_chronolog.c` — the `ldmsd_store` plugin glue (includes `ldmsd.h`).
- `src/chronolog_bridge.{h,cpp}` — owns the shared ChronoLog `Client`, the
  per-story handles, and the set→JSON serialization. It includes only the
  C++-safe `ldms.h` (for the metric accessors) plus the ChronoLog headers.

A single ChronoLog `Client` connection is shared by every storage policy; each
policy holds its own acquired story.

## Building

The plugin is part of the ChronoLog superbuild but, because it depends on the
external OVIS/LDMS development headers, it is built **only when those headers
are found**. Point the build at an LDMS/OVIS install:

```bash
# via the ChronoLog deploy script (environment hint):
LDMS_PREFIX=$HOME/ldms-install tools/deploy/local_single_user_deploy.sh -b -t Debug

# or a direct CMake configure:
cmake -DLDMS_PREFIX=$HOME/ldms-install ...
```

When `ldmsd.h` is not found the plugin is skipped with a status message and the
rest of ChronoLog builds unchanged. The build produces `libstore_chronolog.so`
in the ChronoLog lib dir; add that directory to `LDMSD_PLUGIN_LIBPATH` (or
copy/symlink the `.so` into the ldmsd plugin dir) so `ldmsd` can
`load name=store_chronolog`.

## Configuring ldmsd

See [`examples/ldmsd_store_chronolog.conf`](examples/ldmsd_store_chronolog.conf)
for a complete, ready-to-adapt recipe. In short:

```
load name=store_chronolog
config name=store_chronolog [client_conf=<path>]
strgp_add name=sp plugin=store_chronolog container=ldms schema=meminfo
strgp_prdcr_add name=sp regex=.*
strgp_start name=sp
```

`client_conf` is an optional ChronoLog client config JSON describing the
ChronoVisor portal; omit it to use the client defaults (127.0.0.1:5555).

## Round-trip example (record → tail read → archive read)

[`examples/chrono_ldms_roundtrip_example.cpp`](examples/chrono_ldms_roundtrip_example.cpp)
(installed as `chrono-ldms-example-roundtrip`) is self-contained: it **records**
a handful of LDMS-shaped metric samples (exactly as `store_chronolog` emits them
when an L2 aggregator routes metric sets to it), then reads them back via **both**
paths —

- **tail read** — `StoryHandle::playback()`, straight from the keepers'
  in-memory tail. It **verifies** the payloads read back match exactly what it
  wrote and **polls** until they are sealed (up to `max_settle_seconds`), so the
  process **exits non-zero if the round trip does not verify**. This half needs
  only the keeper/portal (no player).
- **archive read** — `Client::ReplayStory()`, over the player / query service.
  Best-effort and informational only (see the note below); never affects the
  exit code.

```bash
chrono-ldms-example-roundtrip -c <chrono-client-conf.json> ldms meminfo 10 90
#   positional args: [container] [schema] [count] [max_settle_seconds]
```

Because it writes its own data (each sample tagged with a per-run id so repeated
runs stay verifiable), it runs against a fresh deployment with no ldmsd needed.
The tail-read half is why the plugin lives on the tail-read development line
(keeper-side `playback()` API).

> **Note on the archive read for small runs.** On this tail-read build a sealed
> chunk stays resident in the keeper tail (where `playback()` serves it) and is
> only forwarded to the grapher/player archive once it is *evicted* from the
> tail (capacity-driven, default 65536 events/story). A small example run's
> recent samples are therefore readable via the tail but not yet via the
> archive, so `ReplayStory()` returns 0 until they age out of the tail and are
> extracted. The example prints this explicitly; it is expected behavior, not a
> failure of the archive-read call.

## Benchmarks

For the deployment topology and read patterns this integration is sized
against, see [`LDMS-ARCHITECTURE.md`](./LDMS-ARCHITECTURE.md). Write and
tail-read throughput for the ChronoLog path itself is measured by
`chrono-bench` (see the benchmark section of the repo-root `CLAUDE.md`).
