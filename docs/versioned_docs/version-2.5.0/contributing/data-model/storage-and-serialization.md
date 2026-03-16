---
sidebar_position: 4
title: "Storage and Serialization"
---

# Storage and Serialization

ChronoLog uses two serialization mechanisms: Thallium RPC serialization for inter-process communication and HDF5 for persistent storage. This page describes how data types are adapted for each mechanism and documents the ingestion and extraction queues that connect the pipeline stages.

## Serialization Mechanisms

### Thallium RPC Serialization

All core data types (`LogEvent`, `StoryChunk`, `ServiceId`, identity cards, and RPC messages) implement a templated serialization method:

```cpp
template<typename SerArchiveT>
void serialize(SerArchiveT& serT);
```

This method is invoked automatically by the Thallium RPC framework when objects are sent between processes (ChronoKeeper → ChronoGrapher, ChronoVisor → clients, etc.). The Thallium serialization handles standard library containers (`std::map`, `std::vector`, `std::string`, `std::tuple`) transparently, so nested structures like the `StoryChunk`'s event map serialize without custom code.

### HDF5 Persistent Storage

StoryChunks are persisted to disk using the HDF5 file format. Because HDF5 requires fixed-layout types and handles variable-length data differently from `std::string`, ChronoLog defines HVL (HDF5 Variable Length) variants of the core types for storage operations.

## HDF5 Data Types

### LogEventHVL

A storage-oriented variant of `LogEvent` where the `logRecord` field uses HDF5's `hvl_t` type instead of `std::string`. This allows the opaque payload to be written directly to HDF5 variable-length datasets.

| Field | Type | Description |
|---|---|---|
| `storyId` | `uint64_t` | Story this event belongs to |
| `eventTime` | `uint64_t` | Event timestamp |
| `clientId` | `uint32_t` | Client process identifier |
| `eventIndex` | `uint32_t` | Per-client counter |
| `logRecord` | `hvl_t` | Variable-length payload (HDF5 native) |

`LogEventHVL` implements comparison operators (`==`, `!=`, `<`) following the same ordering semantics as `LogEvent` — comparing by `(eventTime, clientId, eventIndex)` for sorting and by `(storyId, eventTime, clientId, eventIndex)` for equality. It manages its own memory for the `hvl_t` payload through proper copy constructors and assignment operators.

**Source:** `chrono_common/include/LogEventHVL.h`

### StoryChunkHVL

The HDF5 counterpart of `StoryChunk`, storing `LogEventHVL` events instead of `LogEvent` instances. It shares the same structure (chronicle name, story name, story ID, time window, revision time) and uses `std::map<EventSequence, LogEventHVL>` as its sorted event container.

StoryChunkHVL introduces two additional types for HDF5 indexing:

- **`EventOffsetSize`** — `std::tuple<uint64_t, uint64_t>` — represents the byte offset and size of an event within an HDF5 dataset.
- **`OffsetMapEntry`** — pairs an `EventSequence` key with an `EventOffsetSize` value, enabling efficient indexed lookups into stored datasets.

**Source:** `chrono_common/include/StoryChunkHVL.h`

### ChunkAttr

Metadata stored alongside each chunk in the HDF5 file:

| Field | Type | Description |
|---|---|---|
| `startTimeStamp` | `uint64_t` | Chunk start time |
| `endTimeStamp` | `uint64_t` | Chunk end time |
| `datasetIndex` | `uint` | Index within the HDF5 file |
| `totalChunkEvents` | `uint` | Number of events in the chunk |

**Source:** `ChronoStore/include/chunkattr.h`

## Ingestion and Extraction Queues

The pipeline between RPC arrival and persistent storage is connected by thread-safe queue structures that decouple producers from consumers.

### StoryChunkIngestionHandle

A double-buffered queue for feeding StoryChunks into the data store. It maintains two `std::deque<StoryChunk*>` instances — an **active** deque and a **passive** deque — protected by a shared mutex.

Multiple RPC service threads push incoming chunks onto the active deque via `ingestChunk()`. A consumer thread periodically calls `swapActiveDeque()` to atomically swap the active and passive deques. The consumer then drains the passive deque without contending with producers. This double-buffering pattern minimizes lock contention between high-throughput ingestion and sequential processing.

**Source:** `chrono_common/include/StoryChunkIngestionHandle.h`

### StoryChunkIngestionQueue

Maps `StoryId` to `StoryChunkIngestionHandle` instances via `std::unordered_map<StoryId, StoryChunkIngestionHandle*>`. When a chunk arrives, the queue looks up the appropriate handle by story ID and delegates to it.

If a chunk arrives for an unknown story (before its handle has been registered), it is placed in an **orphan queue**. The orphan queue is periodically drained via `drainOrphanChunks()`, which re-attempts to match orphaned chunks with newly registered handles. This handles the race condition where chunks may arrive before the story acquisition process completes.

**Source:** `chrono_common/include/StoryChunkIngestionQueue.h`

### StoryChunkExtractionQueue

A mutex-protected FIFO `std::deque<StoryChunk*>` for chunks that are ready to be persisted. The ChronoGrapher's data store pushes merged, finalized chunks into this queue via `stashStoryChunk()`. A dedicated writer thread pops chunks via `ejectStoryChunk()` and writes them to HDF5 storage.

**Source:** `chrono_common/include/StoryChunkExtractionQueue.h`

## Data Flow

The complete path from event arrival to persistent storage:

```
RPC arrival
  → StoryChunkIngestionQueue (route by StoryId)
    → StoryChunkIngestionHandle (double-buffered active/passive)
      → DataStore merge (combine partial chunks)
        → StoryChunkExtractionQueue (FIFO)
          → StoryChunkWriter
            → HDF5 file
```

At each stage, the data representation evolves: raw `LogEvent` objects in `StoryChunk` containers flow through the in-memory pipeline, and are converted to `LogEventHVL` / `StoryChunkHVL` representations at the persistence boundary for HDF5 storage.
