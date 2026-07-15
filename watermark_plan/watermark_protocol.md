# Watermark Feedback Protocol (Keeper ↔ Grapher ↔ Player)

Engineering note for the durability-gated retention that replaces ack-less
story-chunk deletion in the keeper. Companion to the implementation plan
(`watermark_feedback_impl_plan.md`); this documents the protocol as built.

## The problem it solves

Before this change the keeper's drain loop deleted every sealed story chunk
unconditionally — **even chunks whose transfer to the grapher had failed** —
and the grapher acked the byte count *before* writing the events to HDF5. A
transient grapher outage therefore silently destroyed every chunk drained
during it, and the player's hot/cold replay split was a wall-clock guess
(`now − acceptance_window`) served from a best-effort push mirror where a lost
push was only logged.

The fix makes chunk retention **durability-gated**: a keeper frees a chunk only
once its events are confirmed on disk, where "confirmed" is a persisted
watermark `W` the grapher publishes back to the keepers. The player splits
replay at that same durability frontier, pulling the hot portion from the
keepers on demand.

## The invariant

> **E ≤ W** — a keeper never frees a chunk whose events are not confirmed
> durable in HDF5.

`W` is the grapher's per-story persisted watermark; `E` is the highest tick up
to which a keeper has freed chunks. Every design choice below preserves `E ≤ W`.
A stale view of `W` causes only extra retention or redundant re-reads (cleaned
by `EventSequence` dedup) — never data loss.

## Vocabulary

| Symbol | Meaning | Owner | Storage |
|---|---|---|---|
| `W` | End of the **contiguous prefix** of persisted merged timeline windows for a story: every merged window from the story start up to `W` is written+flushed to HDF5. | Grapher | in-memory (a restart resets it — accepted) |
| `E` | Highest tick up to which a keeper has freed retained chunks (bookkeeping only — eviction is per-chunk, never per-story). | each Keeper | in-memory, per story |
| hot floor | Oldest event tick still retained in a keeper's store for a story. | each Keeper | derived from the retention store |
| `B` | Player's replay split boundary = `min(hot_floor)` over the story's keepers. | Player | per query |

## Chunk lifecycle on the keeper

A sealed chunk is owned by the `KeeperChunkRetentionStore` from seal until it is
freed. The **single free condition**, checked by one helper from every mutating
path, is:

```
shipped (grapher acked)
  AND chunk.endTime ≤ known_W (a report covering it arrived)
  AND tail released (no indexed events remain)
  AND not sitting in the extraction queue (no re-send in flight)
```

```
[*] --> RETAINED            : sealed by the pipeline (ingestSealedChunk takes
                              ownership, indexes the tail, AND stashes the ptr
                              to the extraction queue — ship-on-seal)
RETAINED --> SHIPPED_AWAITING_W : grapher ack (markShipped); never freed here
RETAINED --> RETAINED       : transfer fail (markSendFailed); stays retained,
                              readable, re-sent by the stall timer
SHIPPED_AWAITING_W --> SHIPPED_AWAITING_W : stall (W < endTime past resend
                              timeout) → requeueStalled re-stashes, idempotent
                              re-send
SHIPPED_AWAITING_W --> [*]  : freed — W ≥ endTime AND tail released
```

Ship-on-seal means the chunk's pointer enters the extraction queue the instant
it seals; tail eviction only trims the read index and never frees or re-stashes.
`requeueStalled` skips a watermark-covered chunk only if it was acked — an
unshipped chunk below `W` is the straggler case and must re-send (the grapher
re-opens a past window for it).

## When W advances, and why stragglers can't be hurt

1. **W advances only on persistence of the grapher's *merged* windows, over a
   contiguous prefix.** The grapher does not persist keeper chunks directly:
   all keepers' stripes merge into one `StoryPipeline` timeline whose head
   window seals only after the grapher's acceptance window (180 s default) has
   passed beyond the window's end — the designed absorption time for slow
   keepers. On a successful HDF5 write+flush of merged window `[s,e)` the
   extractor calls `advancePersisted(story, s, e)`; the registry records the
   interval and sets `W` = end of the longest contiguous run of persisted
   intervals anchored at story start. A plain `max(end)` would be unsound: the
   grapher drains on 2 streams so windows persist out of order, and a failed
   write must hold `W` back ("never lie" beats "never stall"). **Empty idle-gap
   windows advance `W` vacuously** (nothing to write) so the prefix never
   stalls at a gap where a story went quiet.

2. **A keeper frees per chunk, never per story.** `W` alone frees nothing. If a
   keeper's chunk failed to transfer (or its ack was lost) it has no `shipped`
   mark, retains it, and re-sends on the stall timer — no matter how far other
   keepers' data pushed `W`. One keeper's persistence can never drop another
   keeper's undelivered data.

3. **Why `acked && W ≥ end` implies durable:** the chunk's events merged into
   timeline windows `≤ chunk.endTime` *before* those windows sealed (seal is
   ≥ acceptance-window after the window end); `W ≥ chunk.endTime` means every
   covering merged window persisted (contiguity), so the chunk's events are on
   disk.

4. **Stragglers are never silently dropped.** `StoryPipeline::mergeEvents`
   prepends timeline chunks to extend the timeline into the past and grows the
   acceptance window; the re-opened window later seals and persists as an
   additional rotated HDF5 file (duplicates possible — read-side dedup cleans
   them). The registry ignores persisted intervals at or below `W`, so `W`
   never regresses. The one former discard path — prepend failure — now wraps
   the un-mergeable events into a fresh **watermark-exempt** salvage chunk
   stashed straight to the extraction queue instead of erasing them (exempt so
   its interval, one keeper's rescued events rather than a merged window, does
   not bridge a persistence gap).

5. **Re-sent chunks** whose first delivery succeeded (lost ack / stall timer)
   merge into a still-open window where `StoryChunk`'s `map<EventSequence,
   LogEvent>` dedups them for free; if the window closed they take the prepend
   path of rule 4. There is **no** "discard if ≤ W" ingest guard — the grapher
   cannot tell a redundant re-send from a straggler's first delivery, so it
   ingests both.

6. **Re-registration gap coverage is guarded.** When a story is re-acquired at
   `start > W`, the gap `[W, start)` is treated as covered (nothing recorded in
   it) **only when provable idle**: a fresh pipeline, no prior HDF5 write
   failure for the story, and nothing parked above a persistence gap. A live
   pipeline's open windows may hold received-but-unpersisted events, so
   re-acquiring an active story never moves `W`.

## Player replay split (the player never sees E or W)

The player knows only the story's keeper roster (delivered by the visor at
story start via `start_story_recording_with_keepers`). For replay `[start, end)`
it fans `story_range_fetch` out over the roster; each keeper returns its
retained events in range plus its `hot_floor` and `known_W`
(`HotRangeResponse`). Then:

- **B = min(hot_floor)** over keepers that responded. A keeper retaining
  nothing reports `hot_floor = UINT64_MAX` and drops out of the min; an
  unreachable keeper does the same, which only raises `B`.
- **Hot side:** keeper events merged into `map<EventSequence, LogEvent>`
  (cross-keeper dedup free); events with `time < B` dropped (guaranteed
  archive-covered by `E ≤ W`; the archive portion of the same query returns
  them).
- **Cold side:** archive read cut at `[start, min(end, B))`.

Everything below `B` is on disk because a keeper frees a chunk only once it is
durable and frees proceed oldest-first. Overlap (a chunk persisted but not yet
freed shows up in both archive and a keeper response) is the failure-safe
direction — `EventSequence` dedup cleans it. It is a **completeness argument,
not an optimization**: the region above `B` may not be on disk at all, so the
archive read must stop there.

## Failure behaviors

| Failure | Behavior |
|---|---|
| **Transient grapher outage** (`kill -STOP`/network blip) | Keepers retain the chunks they can't ship, WARN past `retention_cap_mb`, and re-send on the stall timer once the grapher returns. `W` catches up, chunks free. No loss; duplicates on disk possible, deduped on read. |
| **Grapher restart** | `W` resets to 0 (in-memory). Keepers re-send everything still retained; the re-sends land as duplicate rotated files, deduped on read. **Out of scope / documented limitation** — the retention store bounds memory via `retention_cap_mb` in the meantime. |
| **Straggler acked but its prepended window not yet persisted** | Between the ack (+ `W` already ≥ its end) and the rotated-file write, only a grapher *crash* loses it. A `-STOP`/`-CONT` outage keeps grapher memory intact. Rule-6 residual risk, accepted; a future hardening can hold reported `W` below re-opened windows. |
| **Keeper crash** | Its unpersisted stripe is lost — replication is the cut-line, out of scope for this protocol. |
| **Mid-story keeper membership change** | The player roster is not refreshed until the next story start. A stale roster only shrinks the hot set and raises `B` for the others' min (archive covers the rest) — never loss. |
| **Failed HDF5 write** | `advancePersisted` is not called and `persistFailed` marks the story; `W` holds at the last contiguous prefix. Keepers retain + cap-warn until a later successful write (via re-send) advances `W`. |

## Configuration knobs

| Key | Component | Default | Meaning |
|---|---|---|---|
| `retention_cap_mb` | keeper `DataStoreInternals` | 512 | Soft cap on retained sealed-chunk memory; WARN on exceed, **never drops data**. 0 disables the warning. |
| `watermark_resend_timeout_secs` | keeper `DataStoreInternals` | 720 | Re-send a retained chunk whose ack or covering watermark never arrived after this long. ≈ 3 × (grapher `acceptance_window` + `story_chunk_duration`). |
| `watermark_report_interval_secs` | grapher `DataStoreInternals` | 1 | How often the grapher pushes dirty per-story watermarks to contributing keepers. |
| `tail_capacity` | keeper `DataStoreInternals` | 65536 | Max most-recent events indexed per story for last-N tail reads (unchanged; independent of retention). |

## Changed / added RPCs

| RPC | Direction | Change |
|---|---|---|
| `receive_story_chunk` | keeper → grapher / player | Gains a trailing `ServiceId reporter` (the sending keeper's `DataStoreAdminService` — where to push watermark reports). The player receiver accepts and ignores it. |
| `report_story_watermarks` | grapher → keeper | **New**, one-way (`disable_response`): `map<StoryId, uint64_t>` of dirty watermarks, coalesced, ~1 Hz, per contributing keeper. |
| `story_range_fetch` | player → keeper | **New**: `(StoryId, start, end, max_events)` → `HotRangeResponse` (retained events in range + `hot_floor`, `known_W`, `truncated`). The player's on-demand hot source. |
| `start_story_recording_with_keepers` | visor → player | **New**: the 4-arg story start plus `vector<ServiceId>` keeper roster. Keeper/grapher keep the original 4-arg `start_story_recording`. |

## Components

| Component | Role |
|---|---|
| `StoryWatermarkRegistry` (grapher, header-only) | Per-story `W` = contiguous-prefix of persisted merged windows. Fed by the HDF5 extractor; snapshots dirty stories for the publisher. |
| `WatermarkReportPublisher` (grapher) | Tracks story → contributing keepers (from received chunks' reporter ids), pushes coalesced one-way reports each data-collection iteration; lazy per-keeper clients, dropped on send failure. |
| `KeeperChunkRetentionStore` (keeper, was `KeeperTailStore`) | Single owner of every sealed chunk. Ship-on-seal, the free condition, `confirmPersisted`, `requeueStalled`, and `fetchRange` (serves the player) all live here behind one mutex. |
| `KeeperHotFetchClient` (player) | Thin lazy client of `story_range_fetch`, cached per keeper endpoint. |
| disposal seam (`StoryChunkExtractionModule::dispose_chunk`) | Shared keeper/grapher extraction module. The keeper chain routes drain outcomes into the retention store (retain on failure, await watermark on success); the grapher chain deletes as before. |

## Out of scope

Atomic HDF5 publication (temp+rename), fsync policy, archive manifest; grapher
**restart** recovery (W re-derivation); dead-keeper / straggler replication;
ingestion backpressure; removing the dual-endpoint extractor / player mirror
(kept as legacy config). Correctness under a stale `W` comes from `E ≤ W` plus
read-side `EventSequence` dedup, not from atomic publication.
