# Watermark feedback protocol — Keeper ↔ Grapher ↔ Player (sequence)

The cross-component view of the `watermark-feedback` branch. Chunk retention on the keeper is **durability-gated** by the grapher's persisted watermark `W`, upholding the invariant **`E ≤ W`** — a keeper never frees a chunk whose events are not confirmed durable in HDF5. The player then splits replay at **`B = min(hot_floor)`** across the story's keeper roster: everything below `B` is guaranteed archived, so the cold (archive) read stops at `B` and the hot side comes from the keepers on demand (`story_range_fetch`). Duplicates from re-sends/overlap are cleaned by read-side `EventSequence` dedup. Companion to `watermark_plan/watermark_protocol.md`. Fonts enlarged ~50%.

```mermaid
%%{init: {"htmlLabels": false, "themeCSS": ".messageText{font-size:18px !important;} .loopText,.loopText tspan{font-size:18px !important;} .labelText,.labelText tspan{font-size:18px !important;} text.actor tspan,.actor{font-size:18px !important;} .noteText,.noteText tspan{font-size:24px !important;} .sectionTitle{font-size:18px !important;} .sequenceNumber{font-size:16px !important;}"}}%%
%% Watermark feedback protocol (Keeper <-> Grapher <-> Player) — durability-gated retention (E <= W) and the player's hot/cold replay split at B = min(hot_floor)
sequenceDiagram
  autonumber
  actor W as Writer client
  participant K as ChronoKeeper (KeeperChunkRetentionStore)
  participant G as ChronoGrapher (StoryWatermarkRegistry)
  participant FS as HDF5 archive
  participant P as ChronoPlayer
  actor R as Reader client

  Note over W,FS: INVARIANT  —  a keeper never frees a chunk whose events are not confirmed durable in HDF5.  E = highest freed tick (keeper),  W = persisted contiguous-prefix watermark (grapher).  Every step preserves E ≤ W.

  Note over W,G: 1) SEAL + SHIP  —  the keeper owns the sealed chunk and ships it, but keeps it (E stays below the chunk)
  W->>K: record events
  K->>K: seal decayed chunk -> retention store OWNS it (ship-on-seal)
  K->>G: receive_story_chunk(bulk, reporter = keeper)   [RDMA]
  G-->>K: ack  ->  markShipped (retained, awaiting W)

  Note over G,FS: 2) MERGE + PERSIST  —  all keepers' stripes merge into one timeline, whose window seals after the grapher acceptance window and is written to disk
  G->>G: mergeEvents() -> one timeline, window [s,e) seals after acceptance_window
  G->>FS: write + flush merged window [s,e)
  G->>G: advancePersisted(s,e) -> W = end of the longest contiguous run from the anchor

  Note over G,K: 3) REPORT + FREE  —  the grapher reports W, the keeper frees everything W now covers (oldest-first)
  G->>K: report_story_watermarks(W)   [RPC, one-way, ~1 Hz]
  K->>K: confirmPersisted(W) -> free chunks with endTime ≤ W

  Note over K,FS: STRAGGLERS and LOST ACKS  —  a keeper re-sends on the stall timer, the grapher re-opens a past window and re-persists it as a rotated file. Read-side EventSequence dedup cleans duplicates and W never regresses, so no straggler is dropped.

  Note over K,R: 4) REPLAY SPLIT  —  the player cuts hot vs cold at B = min(hot_floor) over the keeper roster. Everything below B is guaranteed on disk (frees happen only once durable, oldest-first).
  R->>P: replay(story, [start, end))
  P->>K: story_range_fetch(story, start, end, max_events)   [RPC, fanned out over the keeper roster]
  K-->>P: HotRangeResponse(events in range, hot_floor, known_W)
  P->>P: B = min(hot_floor) over responding keepers
  P->>FS: cold side = archive read [start, min(end, B))
  P->>P: hot side = keeper events, drop time < B (archive-covered), dedup by EventSequence
  P-->>R: merged replay [start, end)
```
