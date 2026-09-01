# chrono-ldms — LDMS store plugin backed by ChronoLog

`store_chronolog` is an LDMS `ldmsd` **store** plugin that writes aggregated
metric-set samples into [ChronoLog](https://github.com/grc-iit/ChronoLog), a
distributed tiered log / time-series store. It is the ChronoLog side of the
LDMS ↔ ChronoLog integration: LDMS aggregators route sampled metric sets to it,
and it appends each sample as a ChronoLog event.

## Data model mapping

| LDMS                                            | ChronoLog             |
| ----------------------------------------------- | --------------------- |
| storage policy `container=`                     | Chronicle             |
| storage policy `schema=` + the sample's producer | Story                 |
| one metric-set sample                           | one Event (JSON line) |

A storage policy covers every producer feeding its container/schema, but the
story is keyed **per producer**: `<schema>_<producer>`, falling back to
`<schema>` alone for a set that carries no producer name. Keying by schema
alone would funnel every producer through one story handle and one mutex — an
L2 aggregator pulling 200 nodes' `meminfo` would queue all 200 samples per
interval behind one lock, and since `store()` runs synchronously on the updtr
thread that backpressure becomes missed updates upstream. Producer names are
sanitized to `[A-Za-z0-9_-]` because archive filenames are built by direct
concatenation (`<chronicle>.<story>.<startSec>.vlen.h5`): a `/` would become a
path separator into a directory that does not exist, and a `.` would break the
grapher's file discovery.

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

A single ChronoLog `Client` connection is shared by every storage policy. Each
policy holds one handle per producer it has seen, acquired lazily on that
producer's first sample; two policies covering the same
container/schema/producer share one refcounted story handle.

## Building

### Requirements

| | |
| --- | --- |
| **OVIS/LDMS ≥ 4.5.1** | `ldmsd_plug_api.h` — and the plugin ABI it declares — first appears in [v4.5.1](https://github.com/ovis-hpc/ldms/releases/tag/v4.5.1) (Oct 2025). It is absent in v4.4.6 and earlier. Verified against v4.5.3. |
| **jansson** development headers | `ldmsd.h` includes `<jansson.h>` unconditionally. The plugin's CMake does not search for it, so it must resolve from a default include path — normally true on any host with OVIS installed, since OVIS requires jansson itself. |

The plugin is part of the ChronoLog superbuild but, because it depends on the
external OVIS/LDMS development headers, it is built **only when those headers
are found**. Point the build at an LDMS/OVIS install:

```bash
# via the ChronoLog deploy script (environment hint):
LDMS_PREFIX=$HOME/ldms-install tools/deploy/local_single_user_deploy.sh -b -t Debug

# or a direct CMake configure:
cmake -DLDMS_PREFIX=$HOME/ldms-install ...
```

Detection gates on `ldmsd_plug_api.h` rather than `ldmsd.h`, so an OVIS that
predates the targeted ABI is named as such instead of failing later with a wall
of unknown-type errors. When the headers are not found the plugin is skipped
with a status message and the rest of ChronoLog builds unchanged. The build
produces `libstore_chronolog.so` in the ChronoLog lib dir; add that directory to
`LDMSD_PLUGIN_LIBPATH` (or copy/symlink the `.so` into the ldmsd plugin dir) so
`ldmsd` can `load name=store_chronolog`.

> **Do not set `CMAKE_C_EXTENSIONS OFF`.** `CMakeLists.txt` sets
> `CMAKE_CXX_EXTENSIONS OFF` but deliberately leaves the C setting at CMake's
> default of ON, so the C glue is compiled as `-std=gnu11`. Under strict
> `-std=c11` glibc hides `pthread_rwlock_t`, and OVIS's own `ovis_thrstats.h`
> (reached transitively from `ldms.h`) fails to compile. The asymmetry between
> the two languages is load-bearing, not an oversight. The C++ side is
> unaffected: g++ defines `_GNU_SOURCE` itself on glibc regardless of `-std`.

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

### Storage policies must not use decomposition

`struct ldmsd_store` has two mutually exclusive delivery paths, and this plugin
implements only one of them:

| strgp | ldmsd calls | store_chronolog |
| --- | --- | --- |
| no `decomposition=` | `store()` — the whole metric set | **supported** |
| `decomposition=<...>` | `commit()` — decomposed rows | **not implemented** |

Adding a `decomposition=` to the storage policy is silently ineffective rather
than fatal: `ldmsd` NULL-checks the entry point (`ldmsd_strgp.c`, the
`strgp->store->api->commit != NULL` guard), so nothing crashes — it logs

```
store plugin "store_chronolog" does not support decomposition commit()
```

and stores nothing at all. If a storage policy is producing no ChronoLog events
while the plugin loaded and connected cleanly, check for a `decomposition=` on
the strgp first.

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

> **Note on the archive read timing.** A sealed chunk stays resident in the
> keeper tail (where `playback()` serves it) and is forwarded to the
> grapher/player archive only when it leaves the tail. Two bounds release it:
> **age-out** after `tail_retention_secs` (default 60) beyond the chunk's end
> time, and **capacity eviction** once the story exceeds `tail_capacity`
> (default 65536 events). A small run hits the first, not the second, so its
> samples are readable via the tail immediately on sealing but only reach the
> archive about a retention window later — `ReplayStory()` returning 0 before
> then is expected, not a failure of the archive-read call. The example prints
> this explicitly and never fails on it.

## Benchmarks

For the deployment topology and read patterns this integration is sized
against, see [`LDMS-ARCHITECTURE.md`](./LDMS-ARCHITECTURE.md). Write and
tail-read throughput for the ChronoLog path itself is measured by
`chrono-bench` (see the benchmark section of the repo-root `CLAUDE.md`).
