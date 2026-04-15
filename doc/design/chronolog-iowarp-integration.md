# ChronoLog on IOWarp: Design Document

## 1. Overview

This document describes the redesign of ChronoLog to run on top of the IOWarp
runtime (Chimaera + CTE).  The goals are:

1. **Delete ChronoVisor entirely.** Chimaera's admin pool, pool routing, and
   container lifecycle replace its orchestration role.
2. **Define four ChiMods** — `chrono_keeper`, `chrono_grapher`,
   `chrono_player`, `chrono_store` — each encapsulating one domain concern.
3. **Delegate all multi-tiered data movement to the CTE.** StoryChunks become
   CTE blobs; tier progression (RAM → SSD → archive) is driven by CTE scoring
   and reorganization, not hand-rolled RDMA drains.

The result is a system that keeps ChronoLog's tiered-merge semantics while
gaining Chimaera's coroutine scheduling, HermesShm zero-copy transport, CTE
automatic data placement, and GPU readiness — all without a bespoke coordinator
process.

---

## 2. What ChronoVisor Did and Why It Can Be Deleted

ChronoVisor served five purposes in the original design:

| Responsibility | IOWarp replacement |
|---|---|
| Client connection management | Chimaera client connect/disconnect is implicit in pool access. No central gatekeeper needed. |
| Chronicle / story metadata | Moved into the `chrono_keeper` ChiMod as pool-scoped container state (or a shared metadata ChiMod). |
| Recording group assignment | Chimaera pool queries (`PoolQuery::Dynamic()`, range, hash) replace random group selection. Clients send tasks to a keeper pool; the runtime routes them. |
| Start/stop story notification | Becomes `StartStoryRecording` / `StopStoryRecording` tasks dispatched to the appropriate ChiMods. |
| Process registry & heartbeats | Chimaera's built-in node management and `kMonitor` method handle health. |

With these responsibilities distributed, there is no remaining role for a
singleton coordinator.  Every RPC that used to originate from or terminate at
ChronoVisor is replaced by a Chimaera task routed through a pool.

---

## 3. Data Model Mapping

### 3.1 Stories and Chronicles → CTE Tags

A **CTE tag** is a logical container for related blobs.  We map:

```
Chronicle  →  CTE tag namespace prefix   (e.g. "chronicle::weather")
Story      →  CTE tag                    (e.g. "chronicle::weather::station_42")
StoryChunk →  CTE blob within that tag   (blob name = "{start_time}_{end_time}")
```

The tag-based grouping gives the CTE enough information to co-locate related
chunks and to answer range queries ("all blobs in tag X with names between T1
and T2").

### 3.2 Events → Shared-Memory Buffers

Individual `LogEvent` records are not stored as CTE blobs.  They are too small
and too frequent.  Instead, events are buffered inside the `chrono_keeper`
ChiMod's container state using HermesShm data structures (lock-free queues,
shared-memory vectors).  When a chunk is sealed, the chunk is written to the CTE
as a single blob.

### 3.3 Blob Scoring and Tier Progression

CTE blob scores map to data temperature:

| Score | Meaning | Tier | Duration |
|---|---|---|---|
| 1.0 | Hot — just ingested | RAM target | seconds |
| 0.5 | Warm — merged, recent | SSD / NVMe target | minutes–hours |
| 0.0 | Cold — archived | HDF5 on filesystem | unbounded |

The `chrono_grapher` ChiMod calls `ReorganizeBlob` to lower a chunk's score
after merging.  CTE's data-placement engine (DPE) moves the blob to the
matching tier automatically.

---

## 4. ChiMod Definitions

### 4.1 `chrono_keeper` — Event Ingestion

**Purpose:** Accept individual log events at high throughput, buffer them in
memory, produce sealed StoryChunks, and place those chunks on the CTE hot tier.

**Container state (in shared memory):**

```
ChronoKeeperContainer {
    // Metadata
    unordered_map<StoryId, StoryPipelineState>  active_stories;

    // Per-story pipeline (double-buffered for zero-contention ingestion)
    struct StoryPipelineState {
        deque<LogEvent>    active_queue;     // writers append here
        deque<LogEvent>    passive_queue;    // collector drains this
        map<chrono_time, StoryChunk*> timeline;
        ChronicleInfo      chronicle_info;
        StoryInfo          story_info;
    };
}
```

**chimaera_mod.yaml:**

```yaml
module_name: keeper
namespace: chronolog
version: 1.0.0

kCreate: 0
kDestroy: 1
kMonitor: 9

# Domain methods
kRecordEvent: 10          # Client → Keeper: append one event
kRecordEventBatch: 11     # Client → Keeper: append batch of events
kStartStoryRecording: 12  # Admin: begin accepting events for a story
kStopStoryRecording: 13   # Admin: seal remaining chunks, stop accepting
kCreateChronicle: 14      # Create a chronicle namespace
kDestroyChronicle: 15     # Destroy a chronicle namespace
kAcquireStory: 16         # Client acquires a story (returns StoryHandle info)
kReleaseStory: 17         # Client releases a story
kShowChronicles: 18       # List chronicles
kShowStories: 19          # List stories in a chronicle
```

**Task descriptions:**

| Task | IN | OUT | Semantics |
|---|---|---|---|
| `RecordEvent` | `story_id`, `event_time`, `client_id`, `event_index`, `log_record` (ShmPtr) | status | Append event to active queue. Sub-microsecond critical path: enqueue only, no I/O. |
| `RecordEventBatch` | `story_id`, `events[]` (ShmPtr to contiguous buffer) | status, count | Batch variant for throughput. |
| `StartStoryRecording` | `chronicle_name`, `story_name`, `story_id`, `attrs` | status | Create a `StoryPipelineState` in container. |
| `StopStoryRecording` | `story_id` | status | Drain remaining events, seal final chunks, place on CTE, remove pipeline. |
| `AcquireStory` | `chronicle_name`, `story_name`, `attrs` | `story_id`, `pool_id` of this keeper | Hash story name → StoryId. Create pipeline if new. Return connection info so client can send `RecordEvent` tasks directly. |
| `ReleaseStory` | `story_id`, `client_id` | status | Decrement reference count. If zero, trigger drain. |
| `CreateChronicle` | `chronicle_name`, `attrs` | status | Register chronicle metadata. |
| `ShowChronicles` / `ShowStories` | filter | list | Metadata query. |

**Background collection (Monitor task):**

The `kMonitor` handler runs periodically and performs the collection cycle that
the original `KeeperDataStore::dataCollectionTask()` did:

1. **Swap** active ↔ passive queues for each story (atomic pointer swap).
2. **Merge** passive-queue events into the story timeline.
3. **Seal** chunks whose time window has elapsed (`story_chunk_duration_secs`
   past the acceptance window).
4. **Place** sealed chunks on the CTE via `PutBlob` with score = 1.0 (hot).
   - Tag = `"chronicle::{chronicle_name}::{story_name}"`.
   - Blob name = `"{start_time}_{end_time}"`.
   - Data = cereal-serialized StoryChunk in a HermesShm buffer.
5. **Notify** the `chrono_grapher` pool that new chunks are available by
   submitting a `MergeChunks` task with the list of blob names and the tag.

---

### 4.2 `chrono_grapher` — Chunk Merging

**Purpose:** Receive partial StoryChunks produced by multiple keeper instances,
merge them into a single time-ordered chunk, and lower the blob score so the CTE
moves data to the warm tier.

In the original architecture, each Recording Group had N Keepers draining
partial chunks to 1 Grapher via RDMA.  Under IOWarp the CTE already stores
those partial chunks as blobs on the hot tier.  The Grapher reads them via
`GetBlob`, merges, writes back a single merged blob via `PutBlob` with a
lower score, and deletes the partials.

**Container state:**

```
ChronoGrapherContainer {
    // Active merge windows keyed by (tag, time_window)
    unordered_map<StoryId, MergeWindowState>  pending_merges;

    struct MergeWindowState {
        TagId              tag_id;
        chrono_time        window_start;
        chrono_time        window_end;
        vector<BlobName>   partial_blob_names;   // from individual keepers
        uint32_t           expected_partials;     // number of keeper instances
        uint32_t           received_partials;
    };
}
```

**chimaera_mod.yaml:**

```yaml
module_name: grapher
namespace: chronolog
version: 1.0.0

kCreate: 0
kDestroy: 1
kMonitor: 9

kMergeChunks: 10          # Merge partial chunks for a story time window
kFlushStory: 11           # Force-merge and archive all pending chunks for a story
kGetMergeStatus: 12       # Query merge progress for a story
```

**Task descriptions:**

| Task | IN | OUT | Semantics |
|---|---|---|---|
| `MergeChunks` | `tag_id`, `story_id`, `partial_blob_names[]`, `window_start`, `window_end` | status, `merged_blob_name` | 1. `GetBlob` each partial from CTE. 2. Deserialize StoryChunks. 3. Merge event maps (union by `EventSequence`). 4. Serialize merged chunk. 5. `PutBlob` with score = 0.5 (warm). 6. `DelBlob` the partials. 7. Submit `ArchiveChunk` to `chrono_store` if the chunk is old enough. |
| `FlushStory` | `story_id` | status | Merge all pending windows for the story. Used on `StopStoryRecording`. |

**Merge algorithm:**

The merge is the same as the original `StoryPipeline::mergeEvents()`:
`StoryChunk` contains a `std::map<EventSequence, LogEvent>` where
`EventSequence = (time, clientId, index)`.  Merging N partial chunks is
`merged.insert(partial.begin(), partial.end())` for each partial — the map's
ordered-insert deduplicates and sorts automatically.

**Trigger model:**

The Grapher is event-driven, not polling.  The Keeper submits `MergeChunks`
tasks after sealing chunks.  The Monitor task handles stragglers: if a merge
window hasn't received all partials within `acceptance_window_secs`, it merges
what it has.

---

### 4.3 `chrono_player` — Read / Replay

**Purpose:** Serve time-range queries over story data, transparently reading
from whatever CTE tier the data currently resides on.

In the original architecture, ChronoPlayer maintained its own in-memory copy of
recent chunks *and* read historical data from HDF5 archives.  Under IOWarp, the
CTE already provides transparent multi-tier access via `GetBlob`.  The Player
no longer needs its own data cache — it simply queries the CTE.

**Container state:**

```
ChronoPlayerContainer {
    // Read-only reference to the CTE client for blob retrieval
    wrp_cte::core::Client  cte_client;
}
```

**chimaera_mod.yaml:**

```yaml
module_name: player
namespace: chronolog
version: 1.0.0

kCreate: 0
kDestroy: 1
kMonitor: 9

kReplayStory: 10          # Retrieve events for a story within a time range
kReplayChronicle: 11      # Retrieve events across all stories in a chronicle
kGetStoryInfo: 12         # Get metadata about a story (time range, event count)
```

**Task descriptions:**

| Task | IN | OUT | Semantics |
|---|---|---|---|
| `ReplayStory` | `chronicle_name`, `story_name`, `start_time`, `end_time` | `events[]` (ShmPtr to serialized vector) | 1. Compute tag name from chronicle + story. 2. `BlobQuery` on the tag to find all blob names whose time windows overlap `[start_time, end_time]`. 3. `GetBlob` each matching blob from the CTE (CTE handles tier resolution). 4. Deserialize StoryChunks. 5. Filter events to the exact `[start_time, end_time]` range. 6. Return merged, time-ordered event vector. |
| `ReplayChronicle` | `chronicle_name`, `start_time`, `end_time` | `events[]` | `TagQuery` for all tags under chronicle prefix, then same as `ReplayStory` for each. |
| `GetStoryInfo` | `chronicle_name`, `story_name` | `earliest_time`, `latest_time`, `chunk_count`, `total_events` | Metadata query over CTE tag/blob index. |

**Key simplification:**

The original Player duplicated the entire ingestion pipeline (chunk ingestion
queue, story pipeline, double-buffering) just to maintain a warm cache for
reads.  This is unnecessary because:

- Hot data (score 1.0) is still on RAM targets — the CTE serves it at memory
  speed.
- Warm data (score 0.5) is on NVMe — sub-millisecond reads.
- Cold data (score 0.0) is on HDF5 archive — the `chrono_store` ChiMod handles
  the translation.

The Player becomes a thin query executor.

---

### 4.4 `chrono_store` — HDF5 Archive Backend

**Purpose:** Provide durable, portable archival of StoryChunks in HDF5 format.
Operates as a CTE-aware storage backend: when the CTE moves a blob to score
0.0, the Store writes it to an HDF5 file; when the Player requests a cold blob,
the Store reads it back.

**Container state:**

```
ChronoStoreContainer {
    std::string              archive_root;      // filesystem root for HDF5 files
    wrp_cte::core::Client    cte_client;        // for registering as CTE target
}
```

**chimaera_mod.yaml:**

```yaml
module_name: store
namespace: chronolog
version: 1.0.0

kCreate: 0
kDestroy: 1
kMonitor: 9

kArchiveChunk: 10         # Write a merged StoryChunk to HDF5
kReadChunk: 11            # Read a StoryChunk from HDF5
kListArchives: 12         # List archived chunks for a story
kDeleteArchive: 13        # Remove an archived chunk
kCompactArchive: 14       # Merge small HDF5 files into larger ones
```

**Task descriptions:**

| Task | IN | OUT | Semantics |
|---|---|---|---|
| `ArchiveChunk` | `tag_id`, `blob_name`, `chronicle_name`, `story_name`, `story_id` | status, `archive_path` | 1. `GetBlob` from CTE. 2. Deserialize StoryChunk. 3. Write to HDF5 using the existing `StoryWriter` logic. 4. File path: `{archive_root}/{chronicle}/{story}/{start}_{end}.h5`. 5. Optionally `DelBlob` from CTE warm tier (data now lives only in HDF5). |
| `ReadChunk` | `chronicle_name`, `story_name`, `start_time`, `end_time` | `chunk_data` (ShmPtr) | 1. Locate HDF5 file(s) covering the time range. 2. Read using existing `StoryReader` logic. 3. Return serialized StoryChunk in shared memory. |
| `CompactArchive` | `chronicle_name`, `story_name` | status | Merge many small `.h5` files for a story into fewer large files. Reduces filesystem overhead for long-lived stories. |

**HDF5 schema:**

Unchanged from the current design.  Each `.h5` file contains a dataset of
variable-length `LogEventHVL` records with attributes for `chronicle_name`,
`story_name`, `story_id`, `start_time`, `end_time`.

**Integration with CTE tiering:**

There are two integration strategies.  We recommend Strategy A for the initial
implementation and Strategy B as a future optimization.

- **Strategy A — Explicit archive tasks.**  The `chrono_grapher` submits
  `ArchiveChunk` tasks to `chrono_store` after merging chunks whose age exceeds
  a threshold.  The Store writes HDF5 and deletes the CTE blob.  The Player
  submits `ReadChunk` tasks to the Store for cold data.  Simple, auditable, uses
  existing HDF5 code directly.

- **Strategy B — CTE custom target.**  Register the Store as a custom CTE
  bdev target.  When the DPE moves a blob to score 0.0, the CTE's `Write` path
  calls into the Store target, which writes HDF5.  `Read` calls from `GetBlob`
  are similarly intercepted.  This makes the archive completely transparent to
  the rest of the system but requires implementing the bdev interface for HDF5.

---

## 5. Data Flow: End-to-End

### 5.1 Write Path

```
Application
    │
    │  client.AcquireStory("weather", "station_42")
    │  → AsyncAcquireStory task to chrono_keeper pool
    │  ← story_id, keeper pool_id
    │
    │  storyHandle.log_event("temp=72.3")
    │  → AsyncRecordEvent task to chrono_keeper pool
    │     (routed by PoolQuery hash on story_id for affinity)
    │
    ▼
chrono_keeper (ChiMod runtime, one or more instances)
    │
    │  Event appended to per-story double-buffered queue
    │  Monitor task fires periodically:
    │    - Swap queues
    │    - Merge events into timeline
    │    - Seal chunks past acceptance window
    │
    │  Sealed StoryChunk:
    │  → CTE PutBlob(tag="chronicle::weather::station_42",
    │                 blob="{start}_{end}", score=1.0, data=chunk)
    │  → AsyncMergeChunks task to chrono_grapher pool
    │
    ▼
chrono_grapher (ChiMod runtime)
    │
    │  GetBlob for each partial chunk from CTE (hot tier, RAM speed)
    │  Merge partial maps → single ordered chunk
    │  PutBlob(same tag, merged blob name, score=0.5)
    │  DelBlob(partial blob names)
    │
    │  If chunk age > archive_threshold:
    │  → AsyncArchiveChunk task to chrono_store pool
    │
    ▼
chrono_store (ChiMod runtime)
    │
    │  GetBlob from CTE (warm tier)
    │  Deserialize StoryChunk
    │  StoryWriter::write → HDF5 file on disk
    │  DelBlob from CTE (data now only in HDF5)
    │
    ▼
HDF5 archive on filesystem
```

### 5.2 Read Path

```
Application
    │
    │  client.ReplayStory("weather", "station_42", t0, t1)
    │  → AsyncReplayStory task to chrono_player pool
    │
    ▼
chrono_player (ChiMod runtime)
    │
    │  BlobQuery(tag="chronicle::weather::station_42")
    │  → returns list of blob names with time ranges
    │
    │  For each matching blob:
    │    if blob exists in CTE (hot or warm):
    │      GetBlob → deserialize → filter events
    │    else (blob archived, not in CTE):
    │      AsyncReadChunk to chrono_store → StoryReader → HDF5
    │
    │  Merge all chunks, sort, return to client
    │
    ▼
Application receives ordered event vector
```

### 5.3 CTE Tier Progression Timeline

```
t=0s    Event arrives at chrono_keeper
t=10s   Chunk sealed, PutBlob score=1.0          → RAM target
t=10s   MergeChunks task submitted to grapher
t=11s   Grapher merges, PutBlob score=0.5        → CTE DPE moves to NVMe
t=300s  Chunk age > archive_threshold
        Grapher submits ArchiveChunk to store
t=301s  Store writes HDF5, DelBlob               → HDF5 on disk only
```

---

## 6. Client Library Redesign

The public API remains almost identical.  The implementation changes from
Thallium RPC to Chimaera task dispatch.

```cpp
class Client {
public:
    int Connect(const std::string &config_path);
    int Disconnect();

    int CreateChronicle(const std::string &name,
                        const std::map<std::string,std::string> &attrs);
    int DestroyChronicle(const std::string &name);

    std::pair<int, StoryHandle*>
        AcquireStory(const std::string &chronicle,
                     const std::string &story,
                     const std::map<std::string,std::string> &attrs);
    int ReleaseStory(const std::string &chronicle,
                     const std::string &story);

    int ReplayStory(const std::string &chronicle,
                    const std::string &story,
                    uint64_t start_time, uint64_t end_time,
                    std::vector<Event> &events);
};

class StoryHandle {
public:
    // Returns event timestamp
    uint64_t log_event(const std::string &record);

    // Batch variant for higher throughput
    void log_events(const std::vector<std::string> &records,
                    std::vector<uint64_t> &timestamps);
};
```

**Implementation changes:**

- `Connect()` initializes the Chimaera IPC manager and obtains pool references
  for `chrono_keeper` and `chrono_player`.
- `AcquireStory()` sends an `AcquireStory` task to the keeper pool.
- `log_event()` sends a `RecordEvent` task.  The task is allocated in shared
  memory, so the event data is zero-copy into the keeper's ingestion queue.
- `ReplayStory()` sends a `ReplayStory` task to the player pool and waits on
  the future.
- `Disconnect()` is a no-op (pool references are released on destruction).

---

## 7. Deployment Topology

### 7.1 Compose File

IOWarp uses a YAML compose file to declare pools at startup:

```yaml
compose:
  # CTE pool — one per node, manages storage targets
  - mod_name: wrp_cte_core
    pool_name: cte_main
    pool_query: local
    pool_id: "512.0"
    targets:
      neighborhood: 1
    storage:
      - path: "ram::hot_tier"
        bdev_type: "ram"
        capacity_limit: "16GB"
      - path: "/mnt/nvme/warm_tier"
        bdev_type: "file"
        capacity_limit: "500GB"

  # Keeper pool — multiple instances for write throughput
  - mod_name: chronolog_keeper
    pool_name: keeper
    pool_query: local
    pool_id: "600.0"

  # Grapher pool — one per node (or shared)
  - mod_name: chronolog_grapher
    pool_name: grapher
    pool_query: local
    pool_id: "601.0"

  # Player pool — one per node for read serving
  - mod_name: chronolog_player
    pool_name: player
    pool_query: local
    pool_id: "602.0"

  # Store pool — one per node for HDF5 archival
  - mod_name: chronolog_store
    pool_name: store
    pool_query: local
    pool_id: "603.0"
```

### 7.2 Scaling

| Component | Scaling model |
|---|---|
| `chrono_keeper` | Horizontal: multiple pool lanes per node, multiple nodes. Events hash-routed by `story_id` for cache affinity. |
| `chrono_grapher` | One lane per node is usually sufficient. Merge is I/O-bound on CTE reads, not CPU-bound. |
| `chrono_player` | Horizontal: read queries are independent. Multiple lanes for concurrent replay requests. |
| `chrono_store` | One per node. HDF5 writes are serialized per-file anyway. `CompactArchive` can run as a background task. |

---

## 8. Migration Path

### Phase 1: Scaffold ChiMods, Retain Logic

1. Create the four ChiMod directories under `chronolog/chimod/` using the
   `MOD_NAME` template.
2. Move existing domain logic (double-buffered queues, merge algorithm,
   StoryWriter/StoryReader) into the ChiMod runtime classes with minimal
   modification.
3. Replace Thallium RPCs with Chimaera task definitions.
4. Delete ChronoVisor entirely.
5. Write a compose YAML for single-node testing.

### Phase 2: CTE Integration

1. Replace the `StoryChunkExtractorRDMA` (Keeper → Grapher drain) with CTE
   `PutBlob` / `GetBlob`.
2. Implement blob scoring and `ReorganizeBlob` calls in the Grapher.
3. Implement the `chrono_store` archive tasks.
4. Remove the in-memory cache from the Player; route all reads through CTE.

### Phase 3: Optimization

1. Batch `RecordEvent` into `RecordEventBatch` for high-throughput clients.
2. Implement `CompactArchive` for long-lived stories.
3. Profile CTE DPE policy and tune scoring thresholds.
4. Enable GPU acceleration for event sorting/merging if needed.

---

## 9. Configuration

### 9.1 Keeper Configuration (via `CreateParams`)

```yaml
keeper:
  max_story_chunk_size: 4096          # max events per chunk
  story_chunk_duration_secs: 10       # time window per chunk
  acceptance_window_secs: 15          # grace period for late events
  inactive_story_delay_secs: 120      # auto-drain idle stories
  collection_interval_ms: 100         # Monitor task frequency
```

### 9.2 Grapher Configuration

```yaml
grapher:
  merge_timeout_secs: 30              # max wait for partial chunks
  archive_age_threshold_secs: 300     # age at which chunks are archived
  warm_tier_score: 0.5                # CTE score for merged chunks
```

### 9.3 Store Configuration

```yaml
store:
  archive_root: "/data/chronolog/archive"
  compact_threshold: 100              # number of small files before compaction
  hdf5_chunk_cache_mb: 16             # HDF5 chunk cache size
```

---

## 10. What Gets Deleted

| Current file / directory | Action |
|---|---|
| `ChronoVisor/` | **Delete entirely.** |
| `ChronoKeeper/` | Refactor into `chimod/keeper/`. Keep pipeline logic, remove RPC boilerplate. |
| `ChronoGrapher/` | Refactor into `chimod/grapher/`. Keep merge logic, remove RDMA drain code. |
| `ChronoPlayer/` | Refactor into `chimod/player/`. Remove ingestion pipeline (no longer caches data). |
| `ChronoStore/` | Refactor into `chimod/store/`. Keep StoryWriter/StoryReader, add ChiMod wrapper. |
| `Client/` | Rewrite internals to use Chimaera task dispatch. Keep public API unchanged. |
| `chrono_common/` | Keep data structures (`StoryChunk`, `LogEvent`, etc.). Remove RPC serialization helpers (replaced by ChiMod task serialization). |
| `default_conf.json.in` | Replace with compose YAML + per-ChiMod YAML configs. |
| All Thallium/Margo RPC code | **Delete.** Chimaera handles transport. |
| `KeeperRecordingService`, `GrapherRecordingService`, `PlaybackService`, etc. | **Delete.** Replaced by ChiMod task handlers. |
| `StoryChunkExtractorRDMA`, `RDMATransferAgent` | **Delete.** CTE handles data movement. |
| `KeeperRegistry`, `ClientRegistryManager`, `ChronicleMetaDirectory` | Move metadata into `chrono_keeper` container state. |
