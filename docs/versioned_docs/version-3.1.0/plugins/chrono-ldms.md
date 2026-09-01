---
sidebar_position: 5
title: "LDMS Store"
---

# LDMS Store

`store_chronolog` is a **store plugin for `ldmsd`**, the daemon of the [OVIS/LDMS](https://github.com/ovis-hpc/ovis) monitoring system. It persists aggregated metric-set samples into ChronoLog: an LDMS aggregator routes matching metric sets to the plugin, and each sample is appended to a ChronoLog Story as one JSON event.

The result is that cluster telemetry lands in the same tiered log as application data, and is readable through the same paths — the low-latency in-memory tail (`playback()`) for live monitoring, and the HDF5 archive (`ReplayStory()`) for historical analysis.

## Where it fits in an LDMS deployment

LDMS samplers run on compute nodes and expose metric sets; L1 and L2 aggregators pull those sets up the tree. An aggregator's **storage policy** (`strgp`) selects which metric sets are handed to a loaded store plugin. `store_chronolog` is that plugin.

```
 Compute nodes            Aggregators                 This plugin              ChronoLog
┌──────────────┐      ┌──────────────────┐      ┌───────────────────┐      ┌──────────────┐
│ ldmsd sampler│      │  L1 aggregator   │      │ store_chronolog.c │      │  ChronoVisor │
│   meminfo    │─────>│  pulls metric    │      │ (ldmsd store glue)│─────>│  ChronoKeeper│
│   vmstat     │      │      sets        │      │        │          │      │  ChronoGrapher│
└──────────────┘      └────────┬─────────┘      │        ▼          │      │  ChronoPlayer│
┌──────────────┐               │                │chronolog_bridge.  │      └──────────────┘
│ ldmsd sampler│───────────────┤                │       cpp         │
└──────────────┘               ▼                │ set → JSON, C++   │
                      ┌──────────────────┐      │      client       │
                      │  L2 aggregator   │─────>└───────────────────┘
                      │ storage policy   │  store(metric set)
                      │ container/schema │
                      └──────────────────┘
```

Because `ldmsd.h` is C-only while the ChronoLog client API is C++-only, the plugin is split across two translation units joined by a small C ABI: `src/store_chronolog.c` holds the `ldmsd` glue, and `src/chronolog_bridge.cpp` owns the ChronoLog client, the story handles, and the metric-set→JSON serialization.

## Data model

| LDMS | ChronoLog |
|---|---|
| Storage policy `container=` | Chronicle |
| Storage policy `schema=` + the sample's producer | Story |
| One metric-set sample | One Event (a JSON line) |

### Story naming

A storage policy covers *every* producer feeding its container/schema, but the ChronoLog Story is keyed **per producer** — the story name is `<schema>_<producer>`. A set that carries no producer name falls back to `<schema>` alone.

This matters for throughput, not just organization. With one story per policy, every producer's sample would contend on a single story handle: an L2 aggregator pulling 200 nodes' `meminfo` would queue all 200 samples per interval behind one lock. Since `store()` runs synchronously on the aggregator's worker thread, that backpressure becomes missed updates upstream — sample loss at the storing node. Keying per producer yields N independent stories that proceed in parallel. A story is acquired lazily, on the first sample seen from each producer.

Producer names are sanitized before use: anything outside `[A-Za-z0-9_-]` is folded to `_`. LDMS producer names are typically hostnames, and both `.` and `/` are unsafe in a ChronoLog story name — archive filenames are built by direct concatenation (`<chronicle>.<story>.<start>.vlen.h5`), so a `/` becomes a path separator into a nonexistent directory and a `.` breaks the grapher's file discovery.

## Event format

Each `store()` call serializes the metrics the storage policy selected into one JSON object:

```json
{"timestamp":1782530790.001967,"producer":"node1","instance":"node1/meminfo",
 "schema":"meminfo","metrics":{"component_id":1,"MemTotal":65467136}}
```

The envelope carries the LDMS transaction timestamp (seconds, with microseconds to six digits) plus the set's producer, instance, and schema names. Metric values map as follows:

| LDMS metric type | JSON |
|---|---|
| Numeric scalars (`U8`…`D64`) | Number |
| `CHAR` | String |
| Numeric arrays | Array of numbers |
| `CHAR_ARRAY` | String (bounded by the array length) |
| `LIST`, `RECORD` | `null` — not yet supported |

Floats are emitted with 9 significant digits and doubles with 17, so a round trip through JSON preserves the value exactly.

## Building

The plugin depends on external OVIS/LDMS development headers, so it is **self-gating**: it builds only when those headers are found, and the rest of ChronoLog builds unchanged when they are not. Point the build at an LDMS/OVIS install with `LDMS_PREFIX`:

```bash
# via the deploy script (forwarded as an explicit -D)
LDMS_PREFIX=$HOME/ldms-install tools/deploy/local_single_user_deploy.sh -b -t Debug

# or directly
cmake -DLDMS_PREFIX=$HOME/ldms-install ...
```

Detection keys on `ldmsd_plug_api.h`, not `ldmsd.h`. The plugin targets the newer OVIS plugin ABI (`ldmsd_plug_handle_t`, `ovis_log`, `struct ldmsd_store ldmsd_plugin_interface`); an OVIS generation that ships `ldmsd.h` without `ldmsd_plug_api.h` is too old, and CMake reports that case explicitly rather than failing later with unknown-type errors. When the headers are absent the plugin is skipped with a status message — the round-trip example is still built.

A successful build produces `libstore_chronolog.so` in the ChronoLog lib directory. Add that directory to `LDMSD_PLUGIN_LIBPATH` (or copy the `.so` into the `ldmsd` plugin directory) so `ldmsd` can load it, and put it on `LD_LIBRARY_PATH` so the ChronoLog client library it links against resolves:

```bash
export LDMSD_PLUGIN_LIBPATH=$HOME/chronolog-install/chronolog/lib:$LDMSD_PLUGIN_LIBPATH
export LD_LIBRARY_PATH=$HOME/chronolog-install/chronolog/lib:$LD_LIBRARY_PATH
```

## Configuring ldmsd

```
# a sampler producing metric sets
load name=meminfo
config name=meminfo producer=node1 instance=node1/meminfo schema=meminfo
start name=meminfo interval=1000000 offset=0

# the ChronoLog store plugin
load name=store_chronolog
config name=store_chronolog client_conf=/path/to/chrono-client-conf.json

# storage policy: container -> Chronicle, schema -> Story
strgp_add name=chronolog_meminfo plugin=store_chronolog container=ldms schema=meminfo
strgp_prdcr_add name=chronolog_meminfo regex=.*
strgp_start name=chronolog_meminfo
```

| Config option | Default | Description |
|---|---|---|
| `client_conf` | client defaults (ChronoVisor portal at `127.0.0.1:5555`) | Path to a ChronoLog client config JSON describing the ChronoVisor portal to connect to. Optional. |

The connection is made eagerly at `config` time so that connection errors surface there rather than on the first sample. A complete, ready-to-adapt recipe ships as `plugins/chrono-ldms/examples/ldmsd_store_chronolog.conf`.

:::note
The ChronoLog client is a process-wide singleton whose endpoint is fixed when it is first constructed. A `client_conf` naming a *different* endpoint than the one `ldmsd` is already bound to fails with `EINVAL` rather than being silently ignored — otherwise the plugin would quietly write to the wrong ChronoVisor. Omitting `client_conf` never overrides an endpoint an earlier configuration established.
:::

## Reading the data back

Stored samples are ordinary ChronoLog events, so both read paths apply:

- **Tail read** — `StoryHandle::playback()` returns the last *N* events straight from ChronoKeeper memory, in milliseconds. This is the path for live monitoring and alerting. See [On-Demand Tail Read](../user-guide/architecture/on-demand-tail-read.md).
- **Archive read** — `Client::ReplayStory()` replays a time range from the HDF5 archive via the player, for batch analytics and reporting.

[ChronoViz](./chronoviz.md) exposes both to Grafana, so an LDMS-fed chronicle can be explored in a dashboard without any export step.

A self-verifying example ships with the plugin as `chrono-ldms-example-roundtrip`. It records LDMS-shaped samples exactly as the plugin emits them, then reads them back through both paths — so it exercises the full round trip against a fresh deployment with no `ldmsd` required:

```bash
chrono-ldms-example-roundtrip -c <chrono-client-conf.json> ldms meminfo 10 90
#   positional args: [container] [schema] [count] [max_settle_seconds]
```

:::note Timing
An event is visible to `playback()` only once its chunk seals — roughly `story_chunk_duration_secs + acceptance_window_secs` (~25s with shipped defaults), unless the keeper has `live_tail_read` enabled. It becomes visible to `ReplayStory()` later still, once the sealed chunk leaves the keeper tail and is archived: after `tail_retention_secs` (default 60) beyond the chunk's end time, or earlier if the story exceeds `tail_capacity` (default 65536 events). Allow for both windows when checking that samples arrived; the round-trip example's `max_settle_seconds` argument exists for exactly this.
:::

## Behavior notes

- **Failed writes are reported.** If `log_event()` fails — no keeper could be chosen, or the send itself failed — `store()` returns `EIO` and records the reason. A dead keeper surfaces as a storage-policy error rather than silently discarding the metric stream.
- **`flush` is a no-op.** ChronoLog flushes on the keeper and grapher side; the client exposes no flush primitive.
- **One connection, many policies.** A single ChronoLog client connection is shared process-wide. Two storage policies covering the same container/schema/producer share one refcounted story handle, and the connection is torn down only when the last plugin instance detaches — terminating one instance cannot pull the client out from under another.
