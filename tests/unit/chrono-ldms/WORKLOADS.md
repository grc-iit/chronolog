# Workloads exercised — Kafka vs ChronoLog as an LDMS storage backend

This file summarizes **what was run** (the workload space and the iterations),
for **both the write/ingest path and the read path** (tail + archive).
For results and analysis see [`REPORT.md`](./REPORT.md); raw data is in
`final.csv` (writes), `read.csv` (reads), and `concurrency.csv` / `results.csv`
(earlier write runs).

## 1. What is being compared

| Backend | Client used | LDMS plugin that uses it |
|---|---|---|
| Kafka 4.1.2 (KRaft, 1 broker, `acks=1`) | **librdkafka 2.13** | `store_kafka` |
| ChronoLog (visor+grapher+keeper+player) | **ChronoLog C++ client** | `store_chronolog` (built this session) |

Both receive **byte-identical, LDMS-shaped JSON metric records** (a
`timestamp/producer/instance/schema` envelope + a metrics object, padded to a
target size). All traffic is loopback (`localhost`).

## 2. WRITE workload dimensions (the matrix)

| Dimension | Values |
|---|---|
| Payload size / record | **256 B, 1 KB, 4 KB, 16 KB** |
| Concurrency (independent writers) | **1, 4, 8** |
| Kafka durability | `acks=1` (leader-acked, async page-cache flush) |
| ChronoLog durability | synchronous per-event keeper ack |
| Events per cell (ChronoLog) | 15k–60k (multi-second runs) |
| Events per cell (Kafka) | 0.3M–4M (so the batched pipeline is timed credibly) |

Full grid = 4 sizes × 3 concurrencies × 2 backends = **24 cells**.

### Metrics captured per cell
- **Server-acknowledged throughput** — events/s and MB/s (primary metric).
  - ChronoLog `log_event()` is a synchronous keeper RPC → loop time is acked.
  - Kafka measured through `rd_kafka_flush()` so every record is broker-acked.
- **Client enqueue rate** (Kafka only) — rate the client queue absorbs records.
- **Per-event latency** (single writer) = 1 / single-writer rate.
- **Completion / errors** — completed writer processes vs requested, error counts.

## 2b. READ workload dimensions (`run_read.sh`)

Mirrors the write matrix (payload size × concurrency) for **two read patterns**:

| Dimension | Values |
|---|---|
| Payload size / record | **256 B, 1 KB, 4 KB, 16 KB** |
| Concurrency (independent readers) | **1, 4, 8** |
| Read pattern — **tail** | most recent **1000** events (dashboard-style recent slice) |
| Read pattern — **archive** | **full historical scan** of the dataset (20 000 events/partition·story) |

- **Kafka read** = librdkafka **consumer** with manual offset control: *archive*
  seeks to beginning → scans to high-watermark; *tail* seeks to
  `high_watermark − 1000`. Tiers are genuine (offset-based). Topics pre-loaded
  with 8 partitions × 20 000 events.
- **ChronoLog read** — *tail* uses the new keeper-tail **`playback(N)`** path
  (a **writer-only** client; gathers last-N sequences from the keepers, selects
  the global last-N, fetches those payloads from keeper memory). *Archive* would
  be `ReplayStory()`, but with the tail feature recently-sealed data is retained
  in the keeper hot tier (deferred extraction) → not in the cold archive, so
  ChronoLog archive is **n/a** for in-tail data. Each reader process writes its
  own story, **holds it ≥35 s** (so the chunk seals into the tail), then reads.

Metrics: read throughput (events/s, MB/s) and **query latency** for tail. Grid =
4 sizes × 3 concurrencies × 2 patterns × 2 backends = **48 cells** (ChronoLog
archive cells are n/a — recent data is hot-tier-resident).

### Read-path discoveries that shaped the method
- **ChronoLog data must seal before it is readable.** A writer must **hold the
  story acquired ≥~30 s** (15 s fails, 30 s works) so the chunk decays/seals into
  the keeper TailStore; only then is it returned by `playback()` (and, before
  this feature, by `ReplayStory`).
- **The old `ReplayStory` tail had a ~2 s fixed query-latency floor and did not
  scale** (reader-mode client; only ~1 of *C* completed). The new `playback()`
  path is **~12–49 ms** and **scales** across concurrent writer-only clients
  (all *C* complete) — it bypasses the query-service/player entirely.
- **HDF5 cold-tier archival never populated** (`output/` empty; grapher logged
  55 *orphaned chunks*); with `playback`, recent reads are served from the keeper
  hot tier by design, making the tail/archive split a genuine hot/cold division.

## 3. Concurrency models tried (and why the methodology evolved)

The benchmark was run in three passes; each pass exposed a property that
changed the methodology:

1. **`run_matrix.sh` — single process, N threads (1/4/8) sharing one client.**
   - Kafka: fine.
   - ChronoLog: **7 of 8 multithreaded cells crashed** with Mercury
     `HG_FAULT` ("No proc set, proc must be set in HG_Register()"). → the
     ChronoLog client is not safe for multi-threaded writes from one process.

2. **`run_concurrency.sh` — N independent single-threaded processes** (each its
   own client / story / Kafka partition; ChronoLog's intended many-writers
   model).
   - Surfaced that **Kafka was so fast the timed window was <20 ms** at small
     payloads → unreliable absolute numbers.
   - ChronoLog **deadlocked at C=8** (processes stuck in `futex_do_wait`),
     stalling the run.

3. **`run_final.sh` — independent processes, robust** (the dataset of record):
   - **Per-process `timeout` guard** so a hung ChronoLog writer can't stall the
     matrix (a partial cell is recorded with `procs_done < procs_req`).
   - **High Kafka event counts** (up to 4M) so its pipeline+flush is timed
     credibly; moderate ChronoLog counts to avoid keeper overload.
   - Aggregate throughput = `Σ events / (max t_end − min t_start)` across the
     overlapping per-process timed regions (wall-clock epochs emitted by each
     writer).

## 4. Cross-checks / validation

- **`kafka-producer-perf-test`** (official Java client): ~500k rec/s (489 MB/s)
  at 1 KB/`acks=1` — ~5× slower than the librdkafka path, confirming the C
  client is much faster (and that librdkafka is the right client to compare,
  since `store_kafka` uses it).
- **Apples-to-apples per-event mode** (`KAFKA_SYNC=1` in `kafka_bench`:
  `batch.num.messages=1`, `linger.ms=0`, `max.in.flight=1`, `acks=1`,
  flush-per-record): isolates batching as the sole cause of Kafka's lead. Kafka
  collapses ~150–180× and ChronoLog then leads ~1.6–2× per event. Data in
  `kafka_sync.csv`; see REPORT.md *Apples-to-apples*.
- **Delivery verification**: Kafka `delivered == N`, `errors == 0` per run;
  ChronoLog events verified to land durably via the keeper's CSV/HDF5 archives
  (from the `store_chronolog` end-to-end test).
- **Read cross-check**: `ReplayStory` results validated against written payloads
  (first event matches); Kafka consumer counts validated against partition
  high-watermarks.

## 5. Related functional workload (earlier in session)

Before the backend microbenchmark, an **end-to-end LDMS pipeline** was run to
validate the `store_chronolog` plugin:

- `ldmsd` **sampler** (meminfo + vmstat, 1 s interval) →
- `ldmsd` **aggregator** (prdcr/updtr pulling the sampler) →
- **`store_chronolog`** storage policy (container=`ldms`, schema=`meminfo`/`vmstat`).
- Verified: JSON events archived by the ChronoLog keeper as
  `ldms.meminfo.*.csv` / `ldms.vmstat.*.h5`.

## 6. Known limitations of the workload

- **Single node, loopback** — no real network; isolates client/backend CPU
  cost, not interconnect behavior. A multi-keeper ChronoLog or multi-broker
  Kafka would change aggregate ceilings.
- **Durability not equalized** — ChronoLog per-event keeper ack vs Kafka
  `acks=1` page-cache. Stricter Kafka settings (`acks=all`, replication, sync
  flush) were not tested.
- **Reads served from the keeper recent tier**, not a true cold archive
  (ChronoLog HDF5 archival did not populate — orphaned chunks), so the ChronoLog
  tail/archive distinction is by query range, not storage tier.
- ChronoLog concurrency was capped at 8 and is **unstable** there (both read and
  write); higher concurrency was not pursued because the client deadlocks.
