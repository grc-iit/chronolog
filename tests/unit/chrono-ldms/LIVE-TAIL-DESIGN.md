# Live tail read — extending `playback()` to include all keeper-resident events

How to make the keeper tail read (`playback(n)`) return **near-real-time**
events instead of events that are ~30 s old, and what it costs.

## TL;DR

- **Today `playback()` only reads *sealed* chunks**, so an event isn't
  returned until its chunk seals — a delay of `chunk_duration + acceptance_window`
  (~25–30 s on the default local deploy). It does **not** read the open chunk.
- A **"live tail"** also reads the **open chunk** (and, if you want, the
  ≤1-tick pending buffer), so recent events show up immediately.
- **You do not need to change ingestion or add a new data structure.** The open
  chunk is *already sorted* (a `std::map` keyed by `EventSequence`). Making the
  tail live is mostly a **read** change, not a write-path change.
- Sorting events at ingest (replacing the deque) would be nearly free in today's
  one-RPC-per-event mode but would **hurt the future batched fast path**, where
  the insert becomes the bottleneck. So don't do that.
- Any live read *inside the acceptance window* is **provisional** — a late event
  can still arrive and sort behind one you already returned. Only the sealed
  tail is final.

## 1. How the keeper stores events today (checking your model)

Your ingestion model is **correct**:

1. `log_event` RPC → the event is `push_back`-ed onto the **active deque**
   (unsorted).
2. Every ~1 s the background data-store thread **swaps** the active and passive
   deques.
3. It then takes the now-passive deque and **merges** its events into the
   **open StoryChunk**, whose events live in a `std::map<EventSequence, LogEvent>`.
   So **events are sorted once they reach the StoryChunk**.

The one thing to correct: **the current tail read does *not* look at the open
(unfinished) StoryChunk.** `tail_get_sequences` / `tail_get_events` read *only*
`KeeperTailStore`, and the TailStore is filled *only* by the seal path
(`extractDecayedStoryChunks` hands a chunk over once
`now > chunk.endTime + acceptance_window`). So playback sees an event only after
its chunk has **sealed**.

```
 log_event RPC ─► [ active deque ]        (unsorted, push_back)
                        │  every ~1s: swap
                        ▼
                 [ passive deque ] ─► merge ─► [ OPEN StoryChunk ]   (sorted map)
                                                     │   in storyTimelineMap
                                                     │   fills over chunk_duration,
                                                     │   then waits acceptance_window,
                                                     │   then SEALS
                                                     ▼
                                            [ KeeperTailStore ]  ◄── playback() reads
                                                                      ONLY here today
```

So: **write-to-visible delay = time to fill the chunk (`chunk_duration`) +
`acceptance_window`.** That is the whole reason for the "delay between
production and consumption."

## 2. What "live tail" means

Return the most recent `n` events **including those still open in the keeper**.
Ranked newest → oldest, the recent events live in three places:

| where | sorted? | how far behind real-time |
|---|---|---|
| pending deque (not yet merged) | **no** | ≤ 1 tick (~1 s) |
| **open StoryChunk(s)** in `storyTimelineMap` | **yes** (`std::map`) | merged, not yet sealed |
| sealed chunks in `KeeperTailStore` | yes | what `playback()` reads today |

## 3. The key realization

**The only unsorted data is the ≤1-second pending deque.** The open chunk is
already sorted. So a live tail is mostly a matter of *reading the open chunk*,
not restructuring how events are ingested.

## 4. Options

### Option A — read the open chunk in `playback()` (recommended; no ingestion change)

Extend `tail_get_sequences`/`tail_get_events` to also walk the open
StoryChunk(s) in `storyTimelineMap` (each a sorted `std::map`, so "last n" is a
trivial reverse-iterate) and merge that with the sealed `KeeperTailStore` tail.

- **A1 (simple):** read the open chunks as-is → the tail is at most ~1 tick
  (~1 s) behind real-time. No write-path impact at all.
- **A2 (fully live):** first trigger the same swap+merge the background thread
  does every tick (drains the pending deque into the open chunk), then read →
  includes every RPC-acked event. The **reader** pays a small, bounded merge;
  **writers are untouched**.
- **Ingestion cost: zero** — the write path is unchanged.

### Option B — sort events at ingest (replace the deque with a sorted structure)

Make ingestion keep events sorted so there's nothing to merge.
- Buys **nothing** over Option A (the open chunk is already sorted).
- Adds cost to the *write* path and **conflicts with the batching roadmap** (§5).
- **Not recommended.**

### Option C — keep the deque, maintain a separate bounded sorted index at ingest

Keep the deque authoritative; additionally insert `(EventSequence → pointer)`
into a small, capacity-bounded "live index" at ingest.
- Reads are cheap and never touch the write locks.
- The write path pays *some* extra cost (more than A, less than B).
- Worth it **only if** tail reads become very frequent (high-Hz dashboards ×
  many stories).

## 5. Performance — and the platform caveat (read this carefully)

There are two separate measurements from **two different machines**. **Do not
add their absolute numbers together.**

### (a) Issue #662 — measured on the **Ares cluster** (distributed, InfiniBand)

Use its **ratios / percentages only**, not its microsecond figures, since Ares
is a different platform (and will be the *first* real distributed testbed).
What it tells us about the ingestion structure:

- In today's **one-RPC-per-event** mode, the enqueue step (the deque push under
  lock) is a **small fraction** of the per-event cost. Most of the per-event
  cost is the Mercury/Argobots RPC round-trip + serialization, *not* the data
  structure. → changing the structure touches only a small slice, so the
  end-to-end impact today is **small**.
- **But** once you **batch** (`batch_1000`, the planned optimization), the
  enqueue becomes the **throughput bottleneck** (it's the large majority of the
  now-tiny per-event handler cost), and a **lock-free deque** is what makes it
  fast. A sorted structure has no easy lock-free form and a higher per-op cost,
  so it would **lower the batched ceiling**. → sorting-at-ingest **fights the
  roadmap**.

### (b) This container — a local Docker microbenchmark on a *different* machine

`ingest_bench.cpp` measures only the **structure delta in isolation** (not the
full keeper path): a **sorted insert costs roughly 1.5–2× a deque push**. That
is a *relative* structure fact valid on this machine only; it is **not
comparable to Ares** and must **not** be added to #662's µs numbers.

### Putting it together (platform-independent conclusions)

- **Option A adds zero ingestion cost on any platform** — it never touches the
  write path. This is the safe choice for Ares and here.
- **Options B/C add write-path cost** that is *small* in today's per-event mode
  but becomes *significant under batching*. If you ever add live-index
  maintenance, prefer **keeping the deque + a bounded index (C)** over
  **replacing the deque (B)**.
- **Measure any ingest-side option on Ares**, not in this container. The local
  figures are directional only: they establish the ordering
  `deque < bounded-index < full-sort` and that the delta is a small multiple of
  a very cheap operation.

## 6. Semantics caveat (unavoidable, all options)

Timestamps are **client-assigned**, and `acceptance_window` exists precisely so
a lagging client's event can arrive late and sort **behind** events already
read. So a live tail is **provisional**: a later `playback()` may include events
inserted before ones it already returned. This is inherent to reading inside the
acceptance window — no data structure fixes it. Only the **sealed** tail is
final. Consumers of a live tail should key on `EventSequence` and treat the
recent window as eventually-consistent.

## 7. Recommendation

Implement **Option A** — have `playback()` also read the open chunk (use A2 if
you want fully-live, deque-drained results). It gives a real live tail with **no
ingestion change and no new write-path data structure**, behaves identically on
this container and on Ares, and avoids the batching-roadmap conflict entirely.
Only consider Option C (bounded live index) later, and only if profiling on
**Ares** shows the per-query open-chunk read is too expensive.

---

**Artifacts:** `ingest_bench.cpp` (local structure microbench — *relative*
numbers only); GitHub issue #662 (Ares *ratios* + the batching roadmap);
current sealed-only tail on branch `tail-read-from-sealed-chunk`
(`KeeperTailStore.h`, `KeeperRecordingService` tail RPCs, `playback()`).
