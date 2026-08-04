# Archive File Manifest — Implementation Plan

## Context

The Player has no durable record of what is in the HDF5 archive directory.
`HDF5ArchiveReadingAgent::initialize()` rebuilds its index by **recursively
walking the archive root** (`createStartTimeFileNameMap()`), and the polling
monitor thread then walks it **again** to establish a baseline — two full walks
on every start — followed by a 5 s periodic scan that stats every file and does
an O(n²) diff against the previous state. The index it rebuilds is only
`(chronicle, story) → (start_time_ns → path)`; it has **no end times**, so
`readArchivedStory()` cannot tell when to stop and must keep *opening HDF5 files*
until it sees an event past the range.

The Grapher has the mirror problem. After the watermark work, the published
watermark `W` (highest contiguous durably-archived tick) is authoritative state,
but `StoryWatermarkRegistry` holds it **in memory only**. A restarted Grapher
resets `W` to zero, so keepers re-send everything they still retain and the
archive accumulates duplicate rotated files. `watermark_protocol.md` names this
as the branch's biggest documented limitation and lists "archive manifest"
explicitly as out of scope — this work is the intended follow-on.

The fix is a **persisted archive manifest**: a durable record of what has been
published, written by the Grapher at publish time, read by the Grapher on
restart (to restore `W`) and by the Player (to skip the rescan and to resolve
replay ranges without opening files).

There is also a **live correctness gap** the manifest is the natural place to
close. Rotated/auxiliary files (`<chronicle>.<story>.<start>.vlen.<n>.h5`) are
produced whenever a window is written a second time, and the Player's index
**deliberately skips them** (`addFileToStartTimeFileNameMap()` treats them as
"auxiliary"). For pure re-sends that is harmless — but the watermark branch's
salvage path writes *un-mergeable* events into exactly such a rotated file
("wrap them into a fresh chunk and stash it straight to the extraction queue so
it persists as a rotated file", `setWatermarkExempt(true)`). Those events exist
**only** in the rotated file, so today they are persisted but never served: the
salvage path is only half-effective. See "Auxiliary/rotated files" below.

## Decisions

Committed by the revamp docs (`revamp_design_qa.md` §12/§14, `revamp_issues_draft.md`
E12 #83a/b/c) as **"Design A"**:

- **Grapher is the only writer**; entry written *after* the file is durable.
- **Player reads it read-only** and builds no index of its own.
- **Grapher re-reads it on restart** for recovery. Keeper never reads it.
- Lives in the **HDF5 archive root**, co-located with the files it indexes.
- Explicitly rejected: "Player builds its own index by scanning/inotify" — this
  **supersedes** the older `revamp_tasks.md` §12 wording.

Decided for this plan:

1. **Include atomic publication** (temp name → `rename`), so the invariant
   *"an entry exists iff the file is durable"* is real rather than aspirational.
   Issue #46 is not implemented today — the Grapher writes straight to the final
   name, so a crash mid-write leaves a partial file a reader can pick up.
2. **Format: append-only JSONL + periodic compacted snapshot**, snapshot swapped
   in by atomic rename. Matches the docs' "append + atomic snapshot swap",
   O(1) per publish, human-inspectable, and trivially tail-followable.
3. **Derive `W` from the manifest on Grapher restart** — fixes the watermark
   branch's biggest limitation.
4. **One manifest for the archive root** (single writer ⇒ no contention).
5. **Record rotated files and de-duplicate on read.** The manifest indexes every
   published file including rotations; the archive read path de-duplicates by
   `EventSequence`. This closes the salvaged-events gap without delivering
   duplicates. See "Auxiliary/rotated files".

## Sequencing

Branch from **`watermark-feedback`** (22 commits ahead of `origin/develop`, 0
behind, unmerged) — the manifest is where `W` becomes durable, so it builds
directly on that work. Suggested branch `archive-file-manifest`, ideally in its
own worktree alongside `.claude/worktrees/watermark-feedback`.

Also commit this plan into the repo as `manifest_plan/manifest_impl_plan.md`,
mirroring the existing `watermark_plan/` convention.

## On-disk design

Two files in the archive root:

```
<archive_root>/archive_manifest.log     # append-only, one JSON object per line
<archive_root>/archive_manifest.json    # compacted snapshot, replaced by atomic rename
```

Record (one line per published file):

```json
{"v":1,"chronicle":"c1","story":"cpu_usage","file":"c1.cpu_usage.1784932560.vlen.h5",
 "start":1784932560000000000,"end":1784932620000000000,"seq":0,"events":42,
 "state":"published","exempt":false}
```

- `start`/`end` — the chunk window (`StoryChunk::getStartTime()/getEndTime()`).
  Events are within the window by construction, so this doubles as the
  **event-time bound the Player currently lacks**.
- `seq` — rotation index (`0` for the base name, `n` for `...vlen.<n>.h5`).
- `state` — `published` | `deleted` | `empty` (and later `superseded`, for
  compaction's atomic swap).
- `exempt` — mirrors `StoryChunk::isWatermarkExempt()`; salvage chunks are real
  files the Player must see, but must **not** advance `W`.
- **`empty` records carry no file.** An empty chunk window writes no HDF5 file
  today but still calls `advancePersisted()` vacuously; recording it keeps the
  contiguous run intact so restored `W` matches pre-restart `W` exactly.

**Invariants**

- An entry is appended only *after* the `rename` that publishes the file ⇒ the
  manifest never references a partial file.
- Losing trailing appends (crash before fsync) makes `W` **under-report**, which
  is safe under `E ≤ W` — keepers simply retain longer and re-send. Over-reporting
  is impossible. This is why fsync-per-append is not required.
- A truncated final line (torn append) is tolerated: the loader discards it.

**Snapshot compaction.** When the log exceeds a threshold, write the full state
to `archive_manifest.json.tmp`, `rename` it over `archive_manifest.json`, then
truncate the log. Driven from the Grapher's existing ~1 Hz
`GrapherDataStore::dataCollectionTask()` loop, next to `theWatermarkPublisher->publish()`.

**`W` restoration.** On startup, load the manifest, group by story, and replay
non-`exempt` intervals into `StoryWatermarkRegistry` — reusing its existing
`absorbContiguous()` prefix logic rather than writing new watermark math.

## Auxiliary/rotated files (why dedup is in scope)

`StoryChunkWriter::getStoryChunkFileName()` rotates **only when the base name
already exists**, so a rotated file is never "part 2" of a chunk — it is always a
*second write of the same story + same window-start-second*. Two kinds occur:

| Producer | Content vs. the main file | Skipping it |
| --- | --- | --- |
| Keeper stall re-send / Grapher-restart re-send | duplicate of the same window | harmless |
| **Salvage chunk** (`mergeEvents` cannot prepend the timeline into the past) | **events that were never merged into the main window** | **loses those events** |

So neither current default is correct on its own:

- **Skip (today's behavior)** silently drops salvaged events.
- **Read them** silently returns duplicates, because the archive path does **no
  de-duplication**: `QueryResponseAgent::addArchivedEventsToQueryResponse()` just
  calls `extractEventSeries()` into a flat `std::vector<Event>`. (The watermark
  branch's *hot* path does dedup via `map<EventSequence, LogEvent>`; the archive
  path does not.)

The manifest resolves this: it records every published file **including
rotations** with their windows, and the read path de-duplicates by
`EventSequence` — the same key the hot path already uses. Rotated files then
become safe to read unconditionally.

An `ArchiveReaders.read_aux_files` knob (default `false`) already exists and
wires through to `readArchivedStory()`. Once dedup lands it stops being a
data-visibility switch and becomes a legacy escape hatch — keep it, but flip the
effective behavior to "always consult the manifest, dedup on read".

## Implementation

Work in this order; each step is independently testable.

**1. Shared manifest type** — new `src/chrono-common/include/ArchiveManifest.h`
   + `src/chrono-common/src/ArchiveManifest.cpp`.
   Record struct, JSONL serialize/parse (use `json-c`, already a `chrono-common`
   dependency via `ConfigurationBlocks.cpp`), append, load, snapshot+swap, and
   `deriveWatermarks()` returning per-story `W`. Model the shared-struct shape on
   `src/chrono-common/include/HotRangeResponse.h`.

**2. Atomic publish** — `src/chrono-common/{include,src}/StoryChunkWriter.{h,cpp}`.
   Write to `<final>.tmp`, flush/close, then `rename` to the final name. Choose a
   temp suffix that does **not** end in `.h5` so the Player's `isValidArchiveFile()`
   (extension check) and the rotation regex both ignore it. `writeStoryChunk()`
   currently returns only `hsize_t file_size` — widen it to also return the chosen
   filename (the manifest needs it).

**3. Grapher writes the manifest** — `HDF5FileChunkExtractor.{h,cpp}`.
   Add `attachArchiveManifest(ArchiveManifest*)` following the existing
   `attachWatermarkRegistry()` pattern; thread it through
   `GrapherExtractionChain::activate()` and construct the manifest in
   `ChronoGrapher.cpp` *before* the extraction module so it outlives the extractors.
   In `process_chunk()`, append the entry in the same three branches that already
   call the registry (success / write-failed / empty). Also update
   `delete_story_files()` / `delete_chronicle_files()` to append `deleted` records.

**4. Grapher restores `W`** — `ChronoGrapher.cpp` + `StoryWatermarkRegistry.h`.
   Load the manifest at startup and seed the registry (new `restoreFromManifest()`
   that sets each story's anchor and absorbs published intervals). Add the
   snapshot-compaction call to `GrapherDataStore::dataCollectionTask()`.

**5. Player reads the manifest** — `HDF5ArchiveReadingAgent.{h,cpp}`.
   - `initialize()`: load the manifest instead of `createStartTimeFileNameMap()`;
     **fall back to the existing recursive scan** if the manifest is missing or
     unparsable (backward compatibility with existing archives).
   - Extend the index to carry `end` per file so `readArchivedStory()` can stop
     without opening the next file.
   - Replace the 5 s full-FS diff with a cheap manifest tail-poll (mtime + byte
     offset). Keep the three existing mutators
     (`addFileToStartTimeFileNameMap` / `remove` / `rename`) as the reconciliation
     path.
   - Startup reconciliation policy `auto`: scan only when the manifest is absent
     or the clean-shutdown marker is missing (i.e. after a crash). A clean restart
     does **no** directory walk — the stated goal.

**6. Config** — `ConfigurationBlocks.{h,cpp}`, `ChronoPlayerConfiguration.cpp`,
   `conf/default_conf.json.in`. Grapher `DataStoreInternals`:
   `manifest_enabled` (default true), `manifest_snapshot_threshold_entries`
   (10000), `manifest_fsync` (false). Player `ArchiveReaders`: `manifest_enabled`
   (true), `manifest_poll_interval_ms` (1000).
   Note the Grapher and Player reach the same directory through **two independent
   keys** (`hdf5_archive_dir` vs `story_files_dir`); document that they must
   agree, and log loudly at startup if the Player finds no manifest.

**7. Rotated files + archive-side dedup** — `HDF5ArchiveReadingAgent.cpp`,
   `QueryResponseTransferAgent.cpp`.
   Index rotated files from the manifest (they carry the same window, so they
   attach to the same `(chronicle, story, start)` key — the index value becomes a
   *list* of files, not one path). De-duplicate on read by `EventSequence`:
   accumulate into a `std::map<EventSequence, Event>` before appending to the
   response instead of `extractEventSeries()` straight into a flat vector.
   This is what makes the salvage path actually readable — verify with a salvage
   case, not just a re-send case.

## Testing

- **New** `tests/unit/chrono-common/archive_manifest_test.cpp` (gtest): record
  round-trip, append+load, snapshot swap, tolerance of a truncated final line,
  `W` derivation incl. `empty` gap records and `exempt` exclusion.
- **New** grapher test (alongside `tests/unit/chrono-grapher/story_watermark_registry_test.cpp`):
  `W` restored from a manifest equals `W` before restart.
- **Extend** `tests/functional/chrono-grapher/chrono_grapher_destroy_files_test.cpp`
  — the best existing model for filename-convention coverage — to assert manifest
  updates on the delete paths.
- **Player**: the two tests in `tests/unit/chrono-player/` are assertion-free
  hand-rolled `main()`s (and the HDF5 one no-ops without `--conf`); add a real
  gtest covering manifest-load vs scan-fallback and end-time-based early stop.
- **Dedup / rotated files**: force a salvage chunk (events too old to prepend)
  and assert replay returns them exactly once; force a re-send of an identical
  window and assert replay returns those events exactly once (no duplicates).
  These are the two halves of the correctness gap and must be tested separately.
- **E2E**: extend `tests/end-to-end/` with a restart test — publish, restart the
  Player, assert replay is correct **and** no recursive scan occurred.
- **Regression guard**: the watermark branch's
  `tests/integration/watermark_loop_test.sh` and `watermark_replay_split_test.sh`
  must stay green.

## Verification

Per `CLAUDE.md`, always through the deploy scripts:

```bash
tools/deploy/local_single_user_deploy.sh -b -t Debug
tools/deploy/local_single_user_deploy.sh -i -t Debug
pkill -9 -f "chrono-(visor|keeper|grapher|player) --config"; sleep 2
~/chronolog-install/chronolog/tools/deploy/deploy_local.sh -d -w ~/chronolog-install/chronolog -k 2 -r 1
```

End-to-end checks:

1. Write with `chrono-bench` / the telemetry writer; confirm
   `archive_manifest.log` grows one entry per `.h5` file and that no `.tmp`
   file is ever indexed.
2. **Grapher restart**: note `W` per story, restart the Grapher, confirm `W` is
   restored from the manifest (log line) and that keepers do **not** re-send
   already-published chunks (no new duplicate rotated files).
3. **Player restart**: restart with a populated archive and confirm from the log
   that no recursive scan ran, then replay a range and compare against the
   pre-restart result.
4. **Crash consistency**: `kill -9` the Grapher mid-write; confirm no partial
   `.h5` is indexed, the truncated log line is discarded, and reconciliation
   recovers cleanly.
5. Run `ctest` plus the two watermark integration scripts.
6. clang-format-18 all changed `.h`/`.cpp` before committing.

## Risks / follow-ups

- **Salvaged events are unreadable today — this is a live bug, now in scope**
  (step 7, and "Auxiliary/rotated files" above). It was originally filed here as
  a follow-up; tracing the code showed the watermark salvage path writes
  un-mergeable events into a rotated file that the Player's index skips, so those
  events are persisted and never served. Reading rotated files without dedup
  would instead duplicate re-sent events, so the two must land together.
- **Reading rotated files changes replay results.** Ranges that previously
  returned nothing for salvaged windows will start returning events. That is the
  intended fix, but it will move numbers in any test that asserts exact event
  counts — check `tests/end-to-end/data-integrity/` when step 7 lands.
- **Not in scope**: compaction (L0→L1 merge). The manifest is explicitly the
  prerequisite that makes its atomic swap possible later — hence the
  `superseded` state being reserved in the schema now.
- `getStoryChunkFileName()` currently directory-scans on *every* write to pick
  the rotation index; the manifest can replace that with a lookup. Small,
  separable win — do it after the core lands.
- Multi-Grapher writers are undiscussed in the docs; this design assumes the
  documented single writer.
