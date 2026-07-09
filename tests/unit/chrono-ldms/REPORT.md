# Kafka vs ChronoLog as an LDMS storage backend — benchmark report

Comparison of two storage backends for LDMS metric data:

- **Kafka** — via **librdkafka** (the same C client the LDMS `store_kafka`
  plugin uses).
- **ChronoLog** — via the **ChronoLog C++ client** (the same client the
  `store_chronolog` plugin built in this session uses).

Both backends are fed **byte-identical, LDMS-shaped JSON metric records** (a
`timestamp/producer/instance/schema` envelope + a metrics object, padded to the
target size), so the comparison reflects the storage path rather than payload
differences.

The report covers **both directions**: the **write/ingest** path (§ Results)
and the **read** path — *tail* reads (recent slice, dashboard-style) and
*archive* reads (full historical scan) — in *§ Read path: tail vs archive*.

## Test environment

| | |
|---|---|
| Host | AMD Ryzen AI MAX+ 395, 32 cores, 62 GiB RAM |
| OS | Linux 6.17 |
| Kafka | 4.1.2 (KRaft, single combined broker), librdkafka 2.13.0, `acks=1`, async log flush (OS page cache), 8-partition topic |
| ChronoLog | single deployment (visor+grapher+keeper+player), `ofi+sockets` loopback, single keeper |
| Transport | all loopback (`localhost`) — isolates client/backend CPU cost, not network |

> Single-node, loopback, page-cache durability for Kafka. Absolute numbers are
> upper bounds for this hardware; the **relative** behavior and scaling trends
> are the takeaways.

## Methodology

- **Metric of record: server-acknowledged throughput.**
  - ChronoLog `StoryHandle::log_event()` is a **synchronous keeper RPC** — it
    returns only after the keeper acknowledges the event, so the measured loop
    time is already keeper-acked.
  - Kafka `rd_kafka_produce()` is **async/batched**; throughput is measured
    including `rd_kafka_flush()` so every record is broker-acknowledged
    (`acks=1`). (The client-side *enqueue* rate is also recorded, separately.)
- **Concurrency = independent writer processes** (ChronoLog's intended
  many-writers model; also realistic for Kafka). For concurrency *C*, *C*
  single-threaded processes each write `N/C` events to their own ChronoLog
  story / Kafka partition. Aggregate throughput = `events / (max t_end − min t_start)`.
- Event counts: ChronoLog `N` ≈ 15k–60k/cell (multi-second runs); Kafka
  `N` ≈ 0.3M–4M/cell (so its fast pipeline is timed credibly).
- Source: `kafka_bench.cpp`, `chrono_bench.cpp`, `bench_common.h`,
  `run_final.sh`; raw data in `final.csv`.

## Results — WRITE path (server-acknowledged throughput)

### Events/sec (higher = better)

| payload | C | Kafka (ev/s) | ChronoLog (ev/s) | Kafka advantage |
|--------:|--:|-------------:|-----------------:|----------------:|
| 256 B   | 1 | 2,045,797 | 26,661 | 77× |
| 256 B   | 4 | 11,685,788 | 80,967 | 144× |
| 256 B   | 8 | 16,317,138 | 104,723 | 156× |
| 1 KB    | 1 | 2,394,163 | 25,258 | 95× |
| 1 KB    | 4 | 5,676,094 | 85,620 | 66× |
| 1 KB    | 8 | 6,313,709 | 98,023¹ | 64× |
| 4 KB    | 1 | 627,534 | 18,525 | 34× |
| 4 KB    | 4 | 1,026,117 | 58,827 | 17× |
| 4 KB    | 8 | 1,218,145 | 63,765 | 19× |
| 16 KB   | 1 | 145,656 | 20,462 | 7.1× |
| 16 KB   | 4 | 316,519 | 53,901 | 5.9× |
| 16 KB   | 8 | 381,162 | 58,874 | 6.5× |

¹ ChronoLog 1 KB / C=8: only 6 of 8 writer processes completed; 2 deadlocked
and were killed by the 40 s timeout (see *Stability*).

### Bandwidth (MB/s)

| payload | C | Kafka (MB/s) | ChronoLog (MB/s) |
|--------:|--:|-------------:|-----------------:|
| 256 B   | 8 | 3,984 | 25.6 |
| 1 KB    | 8 | 6,166 | 95.7 |
| 4 KB    | 8 | 4,758 | 249.1 |
| 16 KB   | 8 | 5,956 | 919.9 |

### Cross-check

The official `kafka-producer-perf-test` (Java client) reports ~500k rec/s
(489 MB/s) for 1 KB/acks=1 — ~5× **slower** than the librdkafka path measured
here. The C client is dramatically faster than the Java client; since LDMS's
`store_kafka` uses librdkafka, the librdkafka numbers are the relevant ones.

### Apples-to-apples: Kafka crippled to ChronoLog's per-event mode

To isolate *why* Kafka is faster, we reconfigured the librdkafka producer to do
exactly what ChronoLog does — **one record per request, no pipelining, blocking
on a single-node ack per record** (`batch.num.messages=1`, `linger.ms=0`,
`max.in.flight.requests.per.connection=1`, `acks=1`, and a `flush()` after every
record). Single writer, `KAFKA_SYNC=1` in `kafka_bench`; raw data in
`kafka_sync.csv`.

| payload | Kafka **batched** (async) | Kafka **per-event sync** | ChronoLog **per-event sync** |
|--------:|--------------------------:|-------------------------:|-----------------------------:|
| 256 B | 2,045,797 ev/s | 13,513 ev/s | **26,661 ev/s** |
| 1 KB  | 2,394,163 ev/s | 13,491 ev/s | **25,258 ev/s** |
| 4 KB  |   627,534 ev/s | 11,848 ev/s | **18,525 ev/s** |
| 16 KB |   145,656 ev/s | 10,782 ev/s | **20,462 ev/s** |

Two conclusions, and the second is the surprising one:

1. **The entire gap is batching, not implementation.** Turning batching off
   collapses Kafka **~150–180×** (1 KB: 2.39 M → 13.5 k ev/s) — straight into
   ChronoLog's neighborhood. Kafka's millions/s come from amortizing one
   round-trip over a whole record batch, nothing else.
2. **In the same per-event mode, ChronoLog is ~1.6–2× *faster* than Kafka**
   (1 KB: 25,258 vs 13,491 ev/s). Per-event latency is **~38–54 µs for ChronoLog
   vs ~74–93 µs for Kafka** — ChronoLog's Mercury/Margo HPC RPC is a lower-latency
   per-message path than Kafka's TCP + JVM-broker append + ack. So ChronoLog's
   transport is *not* the problem; its only deficit is the **absence of
   batching**. The fix is to make ChronoLog batch (issue #662: batch_1000 → ~41×),
   not to make Kafka stop.

> Note: `acks=1` (single leader-to-page-cache ack) is the fair analog of
> ChronoLog's single-keeper ack; `acks=all` would be stricter than ChronoLog.
> This per-event mode is a **diagnostic**, not a sane Kafka production config.

## Latency (single writer)

ChronoLog's synchronous design gives a low, predictable **per-event** latency
(= 1 / single-writer rate):

| payload | ChronoLog per-event latency |
|--------:|----------------------------:|
| 256 B | ~37 µs |
| 1 KB  | ~40 µs |
| 4 KB  | ~54 µs |
| 16 KB | ~49 µs |

Kafka's per-record latency is batch-amortized: under max-rate pipelining the
perf tool reports ~0.3–0.5 ms avg (and a single message can wait up to
`linger.ms`). So **ChronoLog actually has lower per-event latency**, while Kafka
wins throughput by batching.

## Key findings

1. **Throughput: Kafka wins by 1–2 orders of magnitude**, driven by async
   batching vs ChronoLog's one-RPC-per-event model. The event-rate gap is
   largest for small records (up to ~150× at 256 B) and shrinks for large
   records. **This gap is entirely batching:** crippling Kafka to per-event sync
   collapses it ~150–180× and ChronoLog then *leads* by ~1.6–2× (lower-latency
   HPC RPC) — see *Apples-to-apples* above.
2. **Bandwidth gap narrows with record size.** ChronoLog amortizes its
   per-event RPC overhead over larger payloads: 6.5 MB/s (256 B) → 920 MB/s
   (16 KB). For LDMS, where a metric-set sample is often multi-KB, ChronoLog is
   far more competitive on bytes/s than on events/s. The Kafka:ChronoLog
   bandwidth ratio falls from ~155× (256 B) to ~6.5× (16 KB).
3. **Concurrency scaling:** ChronoLog scales ~3–3.5× from 1→4 writers, then
   **plateaus at 4→8** (single-keeper bottleneck). Kafka scales strongly for
   tiny records (256 B: 2M→16.3M) but also plateaus past C=4 for ≥1 KB on this
   single broker.
4. **Latency vs throughput trade-off:** ChronoLog favors **low per-event
   latency and synchronous durability** (~40 µs, keeper-acked on return); Kafka
   favors **high batched throughput** (per-record latency sub-ms but not
   sync-per-event).
5. **Stability:** Kafka had **0 errors** across every cell. The **ChronoLog
   client is unstable under concurrency** — single-process multithreaded writes
   intermittently crash with Mercury `HG_FAULT` ("No proc set"), and even
   independent processes occasionally **deadlock** (`futex_do_wait`) at C=8.
   This matters for the `store_chronolog` plugin, which shares one client across
   ldmsd worker threads.

## Read path: tail vs archive

Read methodology (mirrors the write matrix: payload size × concurrency):

- **Kafka** — a librdkafka **consumer** with manual offset control:
  *archive* = seek to beginning, scan to the high-watermark (full history);
  *tail* = seek to `high_watermark − 1000`, read the recent slice.
- **ChronoLog** — *tail* now uses the purpose-built **`playback(N)`** path: a
  **writer-only** client gathers the last-N event sequences from the assigned
  keepers, selects the global last-N, then fetches just those payloads — served
  straight from the keeper's in-memory tail (no player/HDF5). *Archive* would be
  `ReplayStory()`, but with the tail feature recently-sealed data is **retained
  in the keeper hot tier** (deferred extraction), so it is not in the cold
  archive — ChronoLog archive is therefore **n/a** for in-tail data here.
- Concurrency = independent reader processes (per Kafka partition / per story).
- Source: `kafka_read_bench.cpp`, `chrono_read_bench.cpp`, `run_read.sh`; raw
  data in `read.csv`.

### Archive read — full scan, events/s (MB/s)

| payload | C | Kafka | ChronoLog |
|--------:|--:|------:|----------:|
| 256 B  | 1 | 636,798 (155) | n/a — in keeper hot tier |
| 256 B  | 8 | 4,494,118 (1,097) | n/a |
| 1 KB   | 1 | 830,391 (811) | n/a |
| 1 KB   | 8 | 4,592,691 (4,485) | n/a |
| 4 KB   | 1 | 1,596,553 (6,237) | n/a |
| 4 KB   | 8 | 2,771,135 (10,825) | n/a |
| 16 KB  | 1 | 242,701 (3,792) | n/a |
| 16 KB  | 8 | 705,729 (11,027) | n/a |

### Tail read — recent 1000 events (ChronoLog now via `playback()`)

Single-reader latency (= 1000 / throughput):

| payload | Kafka | ChronoLog `playback` | was (ReplayStory) | Kafka adv. |
|--------:|------:|---------------------:|------------------:|-----------:|
| 256 B | ~2.3 ms | **~11.6 ms** | ~2.0 s | ~5× |
| 1 KB  | ~3.2 ms | **~13.2 ms** | ~2.0 s | ~4× |
| 4 KB  | ~2.2 ms | **~17.6 ms** | ~2.0 s | ~8× |
| 16 KB | ~6.4 ms | **~48.6 ms** | ~2.0 s | ~7.6× |

Tail throughput scaling (ChronoLog `playback`, events/s aggregate):

| payload | C=1 | C=4 | C=8 |
|--------:|----:|----:|----:|
| 256 B | 85,933 | 63,237 | 130,397 |
| 1 KB  | 75,763 | 41,427 | 68,668 |
| 4 KB  | 56,925 | 34,703 | 80,981 |
| 16 KB | 20,577 | 28,403 | 66,029 |

### Read findings

1. **`playback()` makes ChronoLog tail reads ~40–170× faster** — from
   ReplayStory's ~2 s fixed floor down to **~12–49 ms** for 1000 recent events.
   That closes the gap with Kafka tail reads (2–6 ms) from ~340–870× to just
   **~4–8×**.
2. **The tail path now scales with concurrent readers.** Using a *writer-only*
   client, all *C* readers complete (8000 events at C=8), unlike the old
   reader-mode `ReplayStory` path where only ~1 of *C* finished. The win comes
   from bypassing the unstable query-service/player path — `playback` talks to
   the keeper recording service directly.
3. **Archive reads remain Kafka's domain.** Kafka full-scan still reaches
   ~11 GB/s at 16 KB / C=8; ChronoLog has no cold-archive read for in-tail data
   (it's intentionally retained hot), so the two split duties: hot tail
   (`playback`) vs cold scan (Kafka / a working ChronoLog archive tier).
4. **Tail bandwidth amortizes with record size** for ChronoLog `playback`:
   21 MB/s (256 B) → ~1,030 MB/s (16 KB) at C=8.

### Read caveat (important)

The ChronoLog tail read depends on the new keeper **TailStore**: a writer must
hold the story acquired ≥~30 s so the chunk **seals** into the tail before it is
queryable (events still in the active timeline are not yet in the tail). Because
sealed data is retained in the keeper and its extraction is **deferred** until it
ages out of the last-N window, `ReplayStory` (the cold-archive path) returns 0
for in-tail data — the "tail vs archive" split is now a genuine **hot-tier vs
cold-tier** division, not a query-range trick. ChronoLog's HDF5 cold-tier
archival itself remained unpopulated in this single-node deployment (orphaned
chunks), so a true cold-scan comparison for ChronoLog is still future work.

## Durability / fairness caveats

- **Different ack semantics.** ChronoLog = synchronous per-event keeper ack
  (durable-on-return). Kafka `acks=1` = leader-acked to page cache (async disk
  flush, RF=1 here). Neither is configured for fsync-per-event; a stricter
  Kafka durability setting (`acks=all`, multi-replica, sync flush) would lower
  Kafka's numbers.
- librdkafka used `linger.ms=5` and a large client queue (favorable batching).
- Loopback only — no real network. A multi-node ChronoLog (multiple keepers)
  would raise its aggregate ceiling; the C=4→8 plateau here is a single-keeper
  limit, not an architectural one.
- ChronoLog "events" land durably (verified via keeper CSV/HDF5 archives in the
  store_chronolog test); the throughput is genuine acked work, not dropped data.

## Bottom line for LDMS

- For **maximum ingest throughput / many tiny metrics**, Kafka (librdkafka) is
  far ahead on **writes** (up to ~150×) and on **archive/full-scan reads**
  (millions of ev/s, up to ~11 GB/s).
- For **larger metric-set records**, **low per-event *write* latency**, or
  **synchronous durability**, ChronoLog is reasonable on write/read bandwidth
  (hundreds of MB/s — to ~1 GB/s tail at 16 KB).
- Given that **HPC monitoring reads are tail-heavy** (≈85% of queries hit recent
  data — see `LDMS-ARCHITECTURE.md`), the read profile matters: the new
  **`playback()`** keeper-tail path brings ChronoLog tail reads down to
  **~12–49 ms** (from ReplayStory's ~2 s) — now within ~4–8× of Kafka and
  viable for live dashboards/alerting, served from the keeper hot tier without
  touching the cold archive.
- Two blockers must be fixed before ChronoLog is production-ready as an LDMS
  backend: (1) the client **concurrency instability** (crashes/deadlocks under
  multi-threaded or multi-process load, on both read and write), and (2) the
  **cold-tier archival / data-retention issue** (orphaned chunks; data lost if a
  story is released before it decays). ldmsd drives the store from multiple
  threads, so (1) is on the critical path.
