# LDMS production architecture & read patterns

Notes on how LDMS samplers/aggregators are deployed in production HPC systems,
and how the collected data is consumed (tail vs archive reads). Relevant to
choosing/sizing a storage backend (see [`REPORT.md`](./REPORT.md) for the
Kafka vs ChronoLog comparison).

## Roles & topology

- **Sampler** daemon (`ldmsd` with sampler plugins) runs on every monitored
  node and collects metrics locally.
- **Aggregator** daemons pull metric sets from samplers (or from lower-level
  aggregators) over the network via `prdcr`/`updtr`; the top-level aggregator
  runs the storage policies (`strgp`).
- Aggregation is **tiered**: **L1** aggregators pull from samplers; **L2/L3**
  tiers exist to cross network boundaries and reach the final store.

## How they're set up / ratios

| Component | Typical ratio / placement |
|---|---|
| Sampler | **1 per node (1:1)** — one sampler daemon per compute node |
| L1 aggregator fan-in | **site-dependent: tens-to-hundreds of samplers per L1**, scaling toward ~10³ for small metric sets / coarse intervals |
| Aggregation levels | **1–3** (single level on small/mid clusters; 2 levels on 10k+ node systems) |

The binding constraint on aggregator fan-in is simply the **bytes/s an
aggregator must pull and store**:

```
required_throughput ≈ node_count × metricset_bytes ÷ sample_interval
```

So a 1 s, large-metric-set configuration needs many more aggregators than a
60 s, small-set configuration. There is no single canonical "nodes per
aggregator" number — it is derived from interval × set size × node count.

- Small/mid clusters often use a **single aggregation level** — a handful of
  aggregators (a Sandia deployment example uses ~4 service nodes).
- Large systems (10k+ nodes) use **2 levels**: L1 per group/cabinet → L2
  collecting all → storage.

## Where each piece is deployed

- **Samplers → compute nodes.** Must be lightweight (the "L" in LDMS) to avoid
  perturbing applications.
- **L1 aggregators → service / non-compute nodes (NCNs)** inside the high-speed
  network — *not* on compute nodes, to avoid stealing compute cycles / adding
  jitter.
- **Final aggregator + storage → dedicated monitoring/management host or an
  external analysis cluster.** Crossing from the HSN out to the persistence/
  analysis tier is exactly why the L2/L3 tier exists.

## Data consumption: tail reads vs archive reads

There is **no published LDMS-specific percentage**, and the split is genuinely
deployment-dependent. The honest framing:

- Monitoring read workloads are **strongly recency-skewed**. The best-known
  analog is Facebook's **Gorilla**: **≥85% of all queries were for data
  collected in the past 26 hours**. Continuous automated consumers
  (Grafana-style dashboards, alerting, live anomaly detection, ODA control
  loops) repeatedly read the **tail**; archive reads are human-driven
  (post-mortem debugging, capacity planning, ML training) and **bursty**.

| View | Tail reads | Archive reads |
|---|---|---|
| By # of read operations | **dominant (~80–90%+)** — dashboards/alerts poll recent data every refresh | minority |
| By bytes scanned | smaller per query | **larger share** — a single historical query can sweep a wide time range |

### Why this matters for backend choice
- A **tail-heavy** read profile favors a log/streaming-tier model — Kafka
  consumers, or ChronoLog's keeper/recent tier (the low-latency synchronous
  write path measured in `REPORT.md`).
- The minority **archive** reads favor a columnar/file tier — ChronoLog's
  player → HDF5 archive (or LDMS SOS). Kafka itself is not a query engine, so
  archive queries require offloading Kafka topics to object/columnar storage.
- This is the natural tiering both systems embody — analogous to Gorilla's
  "hot in-memory cache + HBase cold store" pattern.

## Storage placement & producer concurrency (fit for ChronoLog)

Where the `strgp` (storage policy) runs determines how many concurrent writers
the storage backend sees — which is decisive for ChronoLog (it scales with
independent writers; see `REPORT.md`).

### Factor-check of the common assumptions

1. **"Only the top aggregator does I/O, so producer concurrency = 1."**
   *Mostly true in the usual topology, but it is a deployment choice, not a law.*
   - The `strgp` runs in whatever daemon you attach it to; the common pattern is
     a single top aggregator → **one storage process** → one backend client.
     This is exactly the single-writer case where ChronoLog is weakest
     (≈25k ev/s @ 1 KB, ~95× behind Kafka).
   - Caveats: (a) ldmsd has a worker-thread pool (`-P/worker_threads`,
     `ev_thread_count`), so `store()` is invoked from multiple threads in that
     one process — but `store_chronolog` shares **one** ChronoLog `Client`
     across them and that client is multi-thread-unstable (`HG_FAULT`), so safe
     concurrency ≈ 1 regardless. (b) With `chronicle=container, story=schema`,
     all sets of a schema funnel through **one** `StoryHandle` (one mutex).
2. **"sampler → L1 → L2(opt) → L3(opt) → storage."** *Correct as the common
   path; tiers are optional and storage attaches wherever the `strgp` lives — it
   need not be the top.* `LDMSD_PRDCR_TYPE_LOCAL` ("producer is local to this
   daemon") lets a single daemon store its **own** sampled sets, so a node-local
   daemon can sample *and* write to ChronoLog with no aggregation tier.
3. **(Implicit) ChronoLog wants high producer concurrency.** *True* — it scales
   ~3.5× from 1→4 independent writers, then plateaus on a single keeper.

### Proposal: 1:1 colocation, skip L2/L3, write direct from node-local L1s

**Feasible?** Yes — `prdcr_type=LOCAL` makes each node an independent ChronoLog
client with its own story → producer concurrency = #nodes, ChronoLog's ideal
shape; it avoids the single-writer bottleneck.

**Costs (why full 1:1 over-rotates):**

1. **Breaks LDMS's lightweight-compute-node principle.** L1s live on
   service/non-compute nodes precisely to keep compute nodes clean; a full
   thallium/mercury/margo/Argobots RPC client on every compute node adds CPU,
   memory, and **application jitter**. Biggest objection.
2. **Moves the fan-in to ChronoLog's server side.** N node-writers → keepers
   must absorb N stories (a single keeper plateaued at ~4 writers → need many
   keepers, ≈one per rack), plus a **visor** that is a single coordination point
   for N clients.
3. **Cross-boundary connections explode** — L2/L3 exist to transit the
   HSN→storage boundary with a few flows; direct-from-node means N flows.
4. **Today's bugs become data-loss at scale** — the retention/orphan-chunk issue
   (data lost unless the story is held ≥~30 s) and the concurrency instability.
5. **Doesn't help reads** — the tail-heavy read profile still hits ChronoLog's
   ~2 s `ReplayStory` latency floor and broken cold archive.

**Quantitatively, full 1:1 is overkill for throughput.** A node emits ~1–100
sets/s; a big cluster aggregates to ~10k–1M sets/s. At ~25k ev/s per writer you
only need *tens* of writers (with headroom + keeper spread), not thousands.

### Recommendation

The direction (more concurrent writers) is right for ChronoLog, but full
per-compute-node 1:1 over-rotates. Sweet spot:

> **Store directly from the L1 tier** — many L1 aggregators on service nodes,
> each running `store_chronolog` as its own ChronoLog client, **skipping
> L2/L3**. Concurrency = #L1 (tens–hundreds), enough to saturate a multi-keeper
> ChronoLog, while keeping compute nodes clean and keeper-side stories /
> cross-boundary flows manageable. Fall back to per-node 1:1 only if the L1
> writer count truly can't reach the needed concurrency (unlikely).

**Concrete plugin fix this exposes:** with many writers, `story=schema` makes
every aggregator writing the same schema acquire the **same** story →
re-serializing the concurrency. Key the story by producer — e.g.
`chronicle=container, story=schema + "/" + producer`
(`ldms_set_producer_name_get()`) → one story per node-per-schema → N independent
stories.

**Prerequisites before production:** fix the client **concurrency instability**
and the **cold-tier retention/orphan-chunk** bug; the **read side** remains the
harder, unsolved problem.

## Read-path concurrency (consumer-driven, multi-reader)

Unlike the write/store path (funneled to ~1 top aggregator), the read path has
**no framework-imposed single-reader limit** — concurrency is consumer-driven
and inherently distributed. Verified facts:

- A metric-set read is a one-shot **RDMA pull** (`zap_read`: "Read metadata and
  the first set in the set array in 1 RDMA read"). It is read-only and
  non-destructive, so many readers can pull the same set concurrently without
  contending for the data (only for the source daemon's NIC/CPU).
- ldmsd is a **listening server**; any client can `dir` + `lookup` + update.
  There is no exclusivity and no mode-lock — a daemon can be pulled-from by an
  upstream aggregator *and* read by end-consumers at the same time.
- Sets carry a **data-generation number + transaction begin/end flags**
  (`ldms_set_data_gn_get`, `LDMS_TRANSACTION_BEGIN/END`), so concurrent readers
  get snapshot-consistent reads and can detect a mid-update set (the
  "consistent" column in `ldms_ls`).

| Path | Concurrency shape |
|---|---|
| **Write / store** | Funneled — typically one top aggregator → producer concurrency ≈ 1 (ChronoLog's weak point) |
| **Read / consume** | Fan-out — N consumers, distributed, RDMA-cheap; bounded only by consumers and where the sets live |

Read concurrency by tier:
- **Live / tail** — consumers connect to whichever daemon holds the set and
  RDMA-read it; distribution follows set placement (all sets on one top
  aggregator → all readers hit one box; sets spread across L1/L2 → distributed).
- **Streaming** — `ldms_msg_subscribe` fan-out: many subscribers, daemon pushes
  to all.
- **Archive** — concurrency is a property of the backend: files (FS-parallel),
  **SOS/DSOS** (distributed multi-reader query servers), Kafka (consumer
  groups), ChronoLog (`ReplayStory`, but weak on reader concurrency/latency).

### Dual-consumer hot/cold topology

Because reads are non-exclusive RDMA pulls and each tier keeps its own replica,
a single topology can serve **two consumer populations at once** — live readers
off the memory tier and history readers off the cold archive:

```
                                  L2 holds recent sets in memory; the ┬ is a
                                  FAN-OUT — two readers pull from L2 in parallel:

 samplers → L1 → L2 ─┬─► Group A   (live / tail reads — RDMA-read L2's sets)
                     │
                     └─► L3 ─► strgp ─► archive ─► Group B   (catch-up / history)
```

How to read it (answering the obvious questions about the old drawing):

- The branch starts at **L2**, not at the words next to it. Both arrows leave
  L2.
- It is a **fan-out (divergence), not a convergence**: from L2, *two independent
  readers* pull — **Group A** (top) and **L3** (bottom). Nothing merges.
- **L3 is downstream of L2 (tiered: L1 → L2 → L3).** L3's `updtr` pulls from L2,
  exactly as Group A does — they are not parallel siblings off L1 here. L3 then
  runs the `strgp` and archives for Group B.
- *Could* L2 and L3 instead both pull directly from **L1** (true parallel
  siblings)? **Yes — that's also legal**, because RDMA reads are non-exclusive,
  so one L1 set can be pulled by many aggregators. It's just a different
  topology: parallel decouples the live and archive paths at L1 (L1 is read
  twice) and removes the extra hop, whereas the tiered form drawn here keeps a
  single consolidation point (L2) that both Group A and L3 read.

- **L2 serves Group A *and* L3 simultaneously.** L3's `updtr` and the Group-A
  consumers both RDMA-read L2's in-memory sets concurrently; reads don't contend
  for data, and the generation/transaction mechanism keeps them consistent. No
  lock prevents this.
- **L3 → archive → Group B is decoupled.** L3 runs the `strgp` and writes the
  store; history consumers query the **store**, not the LDMS daemons, on an
  independent path with the backend's own read concurrency.
- Put live readers on **L2, not L3** — L3 is busy with storage I/O; the recent
  state exists at L1/L2/L3 (replicas), so live reads can come off L2 (or L1).

This is the tail/archive split (~85% recent / ~15% historical) realized at the
**topology** level. Key consequence for backend choice: **serving tail reads
from LDMS memory (L2) means the storage backend only handles the minority
archive/catch-up reads** — bursty and latency-tolerant. That **neutralizes
ChronoLog's read weaknesses** (its ~2 s `ReplayStory` floor and shaky reader
concurrency hit only Group B, never live dashboards). Serving tail reads *from*
ChronoLog's keeper tier instead would eat the 2 s floor on every refresh.

Caveats: (1) size L2's NIC/CPU for upstream pull *plus* Group A, or spread
readers across L1/L2; (2) the archive lags the live tier by L3's interval + store
flush, so Group B's data is intentionally a bit behind; (3) Group B's experience
depends on the backend (SOS/DSOS or Kafka good; ChronoLog the weak link);
(4) set uid/gid/perm if the two groups are different tenants.

## Flow control, backpressure & memory bounds

LDMS aggregation is a **pull + overwrite-in-place** model over **fixed-size set
buffers**. The consequences (verified in `src/core/ldms.h` and
`src/core/ldms_xprt.c`):

- A metric set is a **fixed-size buffer allocated from its schema at creation**
  ("the schema knows how much memory to allocate for the set"). An `updtr`
  update is an **RDMA read** (`zap_read`) of the source daemon's memory into the
  *same* local buffer, and "data will be overwritten the next time the set is
  updated."

### Does a downstream (puller) tier backpressure the upstream (source)? — No
The source (sampler / L1 / L2) writes the latest sample into its fixed buffer on
**its own timer**; the puller (L2 / L3) reads on **its own** timer. The two are
unsynchronized and the source never blocks for a consumer. So a slow L3 does not
slow L2, and a slow L2 does not slow L1 or the samplers — **no application-level
backpressure propagates upstream** (only per-RPC zap/RDMA flow control on a
single transfer, which does not throttle sampling).

### Is memory bounded in all tiers? — Yes
A daemon's set memory = Σ(set sizes), **independent of sampling rate, uptime, or
consumer speed**, because every update overwrites in place. No tier holds a
growing queue of historical samples.
- The optional **set array** (`ldms_schema_array_card_set`,
  `LDMS_SET_F_DATA_COPY`) keeps a *fixed* ring of the N most-recent instances —
  still bounded (N × set size).
- The **only** place memory grows with a backlog is a specific **store plugin's
  internal buffer** at the top aggregator (e.g., a Kafka producer queue, or the
  ChronoLog keeper deque) — that is the storage backend, not LDMS set memory.

#### The buffer model (what is and isn't bigger up the tiers)
Two things are commonly misread here, so to be precise:

- **A set buffer holds one *latest* sample, not a queue of samples.** A set
  (e.g. `node1/meminfo`) is a single fixed buffer of *current* metric values,
  overwritten in place each period. "A sampler buffers multiple metric sets"
  means multiple **distinct** sets (meminfo, vmstat, …), each its own buffer —
  **not** multiple time-samples of one set piling up for a downstream puller to
  drain. There is no per-set backlog.
- **The only ring is the set array, and it is small + fixed + set at the
  producer.** `ldms_schema_array_card_set(schema, N)` makes the set hold the N
  most-recent instances round-robin; default N=1 (pure last-value). It is
  configured on the **sampler's** schema (to cover a few missed beats when a
  puller lags), not grown by aggregators.
- **An aggregator holds *replicas*, each identical in size to the source.** An
  aggregator's `lookup` creates a local set with the **remote's schema**
  (`__ldms_create_set(..., schema_name, ...)`), so the replica is the same size
  at L1, L2, L3 as at the sampler. **A set never gets bigger as it is
  aggregated.**
- **Per-set size is constant across tiers; the *count* grows toward the top.**
  What increases up the chain is the **number of sets** an aggregator holds
  (fan-in) — an L1 replicates ~hundreds of nodes' sets, the top holds
  everything — so total memory and bytes-moved-per-pull grow because there are
  **more sets, not bigger ones**. (That is exactly why `Σ(set sizes)` =
  set_count × fixed_per_set_size has no time dimension.)
- **Pull frequency is per-`updtr` config, not inherently lower up the tiers.**
  Each `updtr` has its own `interval=`; tiers are commonly **equal** (e.g. 1 s),
  sometimes coarser higher up to manage load — a deployment knob, not an
  architectural property. Pulling coarser just **skips more of the sampler's
  beats** unless a deep-enough set array covers the gap.

### Are sets discarded when L2 is slow to pull from L1? — Overwritten, not queued-then-dropped
L1 (or the sampler) keeps writing the latest sample into the **same buffer**; if
L2 pulls slower than L1 updates, intermediate samples are **overwritten and
lost** (lossy last-value), not dropped from a queue. Three mechanisms shape it:
- **`data_gn` (data generation number):** the puller detects whether the set
  changed since its last pull, so it can skip storing duplicates.
- **Transaction begin/end + consistency:** if the RDMA read catches the set
  mid-write, the puller detects the inconsistency and **re-reads** ("reading
  multiple times to obtain the updated copy of the set") — it never stores a
  torn sample, and it never makes the source wait.
- **Set array:** lets the puller grab the last N samples per pull instead of 1,
  *reducing* loss; fall more than N behind and the oldest are still overwritten.

### Does L2 handle it the same when L3 is slow? — Yes, fully symmetric
L2's aggregated replicas are fixed buffers overwritten as L2 pulls from L1; L3
pulls from L2 on L3's schedule; a slow L3 misses intermediate updates (overwrite,
not queue). L2 keeps **no backlog for L3** and never slows for it. Every tier
obeys the same rule: pull + overwrite + fixed buffer + lossy-on-overrun + no
upstream backpressure.

### The one practical exception: the store, not the pulls
At the top aggregator the storage policy's `store()` runs **synchronously on the
updtr/worker thread**. A slow/blocking backend stalls that thread → the `updtr`
falls behind → that aggregator **misses updates** (local sample loss). It still
does not backpressure upstream, but it is why a slow sink degrades into dropped
samples *at the storing node*. This is exactly the risk with `store_chronolog`'s
synchronous per-event `log_event` — and a point in favor of Kafka's async
producer queue, which absorbs bursts in bounded client memory instead.

## Sources

- [LDMS (OVIS) — GitHub](https://github.com/ovis-hpc/ldms)
- [Evaluating and Influencing Extreme-Scale Monitoring (LLNL / CUG 2023)](https://www.osti.gov/servlets/purl/1973194)
- [Large-scale Persistent Numerical Data Source Monitoring — Experiences (Sandia, HPCMASPA 2022)](https://www.sandia.gov/app/uploads/sites/218/2022/08/16_HPCMASPA_Experiences.pdf)
- [Production Application Performance Data Streaming for System Monitoring (ACM TOMPECS)](https://dl.acm.org/doi/fullHtml/10.1145/3319498)
- [Gorilla: A Fast, Scalable, In-Memory Time Series Database (VLDB 2015)](https://www.vldb.org/pvldb/vol8/p1816-teller.pdf)
