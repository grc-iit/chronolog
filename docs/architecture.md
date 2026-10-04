# ChronoLog architecture

This is the map for a new engineer. The specification is [ARCHITECTURE.md](../ARCHITECTURE.md) at the repository root:
every rule there carries an id (I4.1, W10.13, M11.3) and names the test that enforces it. Where this page and
ARCHITECTURE.md disagree, ARCHITECTURE.md wins, and where the code and ARCHITECTURE.md disagree, the code is wrong
(section 1). Grep ARCHITECTURE.md by id rather than reading it front to back.

## Vocabulary

A **chronicle** is a namespace of stories. A **story** is one append-only log, identified by a `story_id` the Catalog
assigns; no component ever derives an id from a name (I3.4). A **writer** acquires a story and appends **events**.
Every event carries three time fields, and none substitutes for another (section 3):

| Field | Assigned by | Used for |
| --- | --- | --- |
| `physical` | the writer, at creation | wall-clock range queries, auditing, OTel alignment |
| `hlc` | the Keeper, on accept | total order and causality |
| `EventId` (story, writer, incarnation, sequence) | the Catalog (ids) and the writer (sequence) | identity, deduplication, retry |

Replay order is `(hlc, writer_id, incarnation, sequence)`, lexicographic, implemented once as `ReplayLess` in
`include/chronolog/types.h` (section 7). Section 2 is the glossary; read it first.

## Four services

```text
                 Catalog, Cluster
   SDK  ───────────►  Visor  ◄──────── heartbeats, routes, acquisitions ────────┐
    │                                                                            │
    │ append                                                                     │
    ▼                                                                            │
  Keeper  ── HLC, WAL fsync, sealed chunks ──►  Grapher  ── archive on local, ──►  slow tiers
    ▲                                              │       manifest per Grapher
    │ FetchHot (recent events, sealed frontier)    │ manifest, archive files
    │                                              ▼
  Player  ◄────────────────────────────────────────┘
    │ Read and Tail, merged, deduplicated, with a Completion
    ▼
   SDK
```

| Service | Binary | Owns | Implements | Source |
| --- | --- | --- | --- | --- |
| Visor | `chrono_visor` | The Catalog (chronicles, stories, ids, writer acquisitions and their leases, revisions) in SQLite, and the Cluster (registered processes, Routes, epochs, ceilings). A replicated deployment runs three Visors over Raft. | MetadataStore, Membership | `src/visor/` (`catalog/`, `membership/`, `dynamic/`, `raft/`, `adapter/`) |
| Keeper | `chrono_keeper` | Ingestion: validates the epoch, assigns the HLC, keeps the WAL, seals frontiers, cuts chunks and ships them to the Grapher, answers FetchHot. | Journal, Clock | `src/keeper/` (`journal/`, `wal/`, `archive/`, `membership/`, `adapter/`) |
| Grapher | `chrono_grapher` | The archive: publishes chunks as files on the `local` tier, keeps its own manifest log, compacts, migrates to slow tiers, scrubs, and frees destroyed stories. | TierStore publish | `src/grapher/` |
| Player | `chrono_player` | Replay: Read and Tail over Keeper data and the archive, merged in total order and deduplicated, ending with a Completion. Stateless. | Replay | `src/player/` (`replay/`, `adapter/`) |

Shared code lives in `src/common/`: `clock/` (HLC core, kernel clock, clock audit), `tier/` (FileTierStore, manifest
logs, chunk codecs, used by the Grapher and the Player), `worker/` (the bounded worker pool) and `rpc/` (the one
channel policy every service uses to reach a peer). Each service has exactly one `main.cpp` that composes the
contracts from configuration (M11.8).

The six contracts are pure C++ interfaces in `include/chronolog/`: Clock, MetadataStore, Membership, Journal,
TierStore and Replay. They return `absl::Status` and contain no gRPC or protobuf types (section 9). Each has a
value-parameterized suite in `tests/contract/` that every implementation instantiates in its own test directory
(section 15). The wire API is `proto/chronolog/v1` for clients and `proto/chronolog/internal/v1` between services;
both may only grow additively (section 10, W10.1).

## Life of an event

**Create and acquire.** A client creates a chronicle and a story at the Catalog and gets a `story_id`. It then
acquires the story under a stable writer identity and receives a `writer_id`, a new `incarnation`, the story's Route
and epoch, and its `assigned_keeper` (I7.5). Every acquisition has a finite lease that the SDK renews in the
background (I3.8). Release fences the incarnation: the Keeper rejects any later append that carries it (I3.7).

**Append.** The SDK sends single-story batches (W10.13). Each item carries a complete EventId with a gapless sequence
(I3.1, I5.5), the epoch, the writer's physical reading and a `causal_floor`, the highest HLC the client has observed.
The Keeper validates the epoch: a stale one is answered per item with FAILED_PRECONDITION and the current Route, and
the client retries with the same EventIds (I4.1, I4.4). It assigns an HLC above the causal floor, so read-then-write
causality holds (I7.3), and it never modifies the physical time (I3.3).

**Durability.** "ACCEPTED: the event is in Keeper RAM and ordered. It is lost on Keeper crash. DURABLE: the event is
fsync'd in the Keeper WAL with group commit. It survives Keeper crash." (section 5). DURABLE is the default and the
only level called an ack. A server never downgrades silently (I5.1), DURABLE means fsync returned success (I5.6),
and the WAL commits groups adaptively (I5.10). A retry with the same EventId returns the original result and stores
nothing new (I5.4).

**Seal.** Each Keeper maintains a sealed frontier F: "every event of w with hlc < F is visible to reads and no event
of w with hlc < F will ever be ingested" (section 2). A pending DURABLE event caps F below its HLC until its fsync
returns (I6.6). Ordering rules for ticking F are in I6.11.

**Transfer.** The Keeper cuts each story into chunks, half-open HLC windows of `story_chunk_duration_secs`, and streams
sealed chunks to the Grapher. The Grapher answers with a receipt, and delivery is not persistence: a stream without
an OK receipt is a failed send and the chunk stays readable and resendable (I12.3), and a covering watermark alone
never settles a receipt (I12.4).

**Archive.** The Grapher publishes each chunk as a file (temporary name, fsync, link, directory fsync) before it
appends the manifest record (I13.1). The persisted watermark W is the end of the contiguous run of persisted windows;
it stops at the first gap (I13.2), counts empty windows (I13.3) and never regresses (I13.6). Each Grapher appends only
to its own manifest log, because concurrent appends to one file on NFS lose records (I13.7, section 12). The Keeper
frees a chunk from RAM only when it is shipped, below W, receipted and past the visibility delay (I13.8), and
truncates the WAL at receipt settlement, never at W alone (I13.10).

**Tiers.** Compaction replaces small contiguous files with one output through a single manifest line (I13.12).
Migration moves old files to slower tiers through a `migrate_v1` line, so every reader sees each file at exactly one
effective location (I13.13, I13.14).

**Replay.** The Player plans a Read from the Route and the merged manifests, fetches recent events from every Keeper
in the Route (FetchHot, which returns the Keeper's sealed frontier) and older ones from the archive, merges them in
total order (I7.1) and deduplicates by EventId across sources (I6.4). A Tail resumes exclusively after a position
`(hlc, EventId)` and never re-sends the event at that position (I6.9).

## Completion, and why it is exact

Every Read ends with exactly one Completion: `complete`, `frontier`, `laggards` and, when incomplete, a `reason`
(section 6). The definition: "the read is complete iff every Keeper in the story's Route answered and its sealed
frontier F_k >= end, on the HLC axis." Because each F_k is a sealed prefix, no event below `end` can arrive after the
answer, so `complete=true` means "every event in the range that any writer will ever produce is in the stream"
(I6.8). The registered-writer view is used only to name laggards, so a stale view cannot produce a false complete.

When the answer is not complete the reason says why, and when several apply the first of this order is reported:

- `SOURCE_FAILED`: a Keeper or an archive file did not answer or could not be read (I6.3, I6.14). Unavailable is not
  lost: a tier that is down makes reads incomplete until it returns (I13.15).
- `TRUNCATED`: the answer hit a bound. Its frontier `c` is a complete prefix: `[start, c)` is complete on its own and
  a continuation from `c` has no gap or duplicate (I6.12).
- `PHYSICAL_AXIS_UNBOUNDED`: a physical-time Read selected an event without a finite clock bound (I6.10, section 8).
- `LAGGING_WRITERS`: some Keeper's seal is below `end`; the laggards are listed (I6.1).

A Tail never claims completeness (I6.5). For DURABLE events a complete Read is stable: a later Read of the same range
returns the same set (I6.8).

## The tier chain

Section 13 opens with the chain: "Keeper RAM (hot, serves Tail and recent Read, bounded by retention_cap_mb); the
Keeper WAL (DURABLE); `local`, the Grapher's archive_root wherever it lives, where every chunk is published; then zero
or more slow tiers of kind `posix` (NFS, PFS, a second disk; `s3` is deferred)." Manifests stay on `local`.

Each tier root holds a `.chronolog-tier.json` marker written only by an explicit tier add (`chronolog tier add`)
(I13.15). Each tier has a byte budget and watermarks; usage above the high watermark or a file older than
`migrate_after_s` starts migration to the next available tier (I13.16). Recovery after a crash is bounded by
validated marks and the scrubber (I13.17).

## Membership and failover

In static mode, the default, every story is epoch 1 and Routes come from configuration (I4.3). In dynamic mode three
Visors replicate the Catalog over Raft, Keepers join, drain and fail over, and epochs change under I4.7 to I4.15:

- Each Keeper instance holds a ceiling committed in the Catalog and never assigns an HLC at or above it (I4.7).
- A route change is one Catalog command that raises the epoch and records an ordering cut H_X; every new-epoch
  assignment of a remapped writer is above H_X (I4.9, I4.11).
- A removed Keeper stays a source, a predecessor, for Reads below its own cut until it drains to the archive (I4.13,
  I4.14). An operator who declares it lost abandons it, and its ranges read SOURCE_FAILED forever (I4.15).

Section 12 is the failure model, one paragraph per failure with its gate: Keeper crash (DURABLE events recovered from
the WAL), Grapher crash (receipts scoped to an instance), Visor crash (SQLite with synchronous=FULL, Raft failover in
dynamic mode), Player crash (stateless; clients resume from their last position), writer crash and release fencing
lag.

Core has no TLS, authentication or event replication and assumes a trusted network; a separate gateway does
authentication and ACLs (section 14).

## Client layers

| Layer | Where | What it adds |
| --- | --- | --- |
| C++ SDK | `client/cpp/` (`chronolog::client`, headers in `client/cpp/include/chronolog/client/`) | Connect, Catalog calls, acquire with lease renewal, append with idempotent retry and route adoption, Read and Tail streams. |
| Context API | `client/cpp/context/` | The agent layer: a context is one story; `remember` with caller-chosen operation ids, `recall` and `latest` over certified pages, `follow`, `reconcile` (LANDED, ABSENT, UNKNOWN), checkpoints. |
| Python | `client/python/` (package `chronolog`, nanobind) | The SDK and the Context API, with typed exceptions. |
| TypeScript | `client/typescript/` (package `@chronolog/client`, N-API) | The same surface with promises and async iterators. |
| Plugins | `plugins/<name>/` | Built only on the public SDK and bindings, never on `src/` (section 11). `plugins/mcp` exposes the Context API to agents. See [plugins/README.md](../plugins/README.md). |
| Launcher | `launcher/` (the `chronolog` CLI) | Runs one node as local processes, tracks instances, adds tiers. Runs the service binaries and never includes `src/`. |

## Where to read next

| Topic | ARCHITECTURE.md |
| --- | --- |
| Terms | section 2 |
| Time fields, envelope, tombstones, leases | section 3 (I3.x) |
| Epochs, dynamic membership | section 4 (I4.x) |
| ACCEPTED and DURABLE | section 5 (I5.x) |
| Completion | section 6 (I6.x) |
| Order | section 7 (I7.x) |
| Clocks and the physical axis | section 8 (I8.x) |
| The six interfaces | section 9 |
| Wire rules | section 10 (W10.x) |
| Module and dependency rules | section 11 (M11.x) |
| Failure model | section 12 |
| Tiers, archive, compaction, migration | section 13 (I13.x) |
| Security boundary | section 14 (S14.x) |
| Test gates and the contract test index | section 15 |
| How contract changes are approved | section 16 (A16.3) |
