---
sidebar_position: 1
title: "Overview"
---

# Data Model Overview

ChronoLog's data model organizes time-series log events from distributed HPC applications into a hierarchical structure. Events flow from client processes through a multi-tier distributed pipeline, ultimately reaching persistent storage. The model is designed to support high-throughput ingestion from many concurrent clients while preserving global event ordering.

## Conceptual Hierarchy

```
Chronicle (namespace/container)
  └── Story (time-series dataset)
        └── StoryChunk (time-windowed event batch)
              └── LogEvent (individual record)
```

An additional concept, **Archive**, provides historical storage associated with a Chronicle.

## Chronicle

A Chronicle is the top-level organizational unit in ChronoLog — a named container that groups related Stories together for easier data management. Each Chronicle is uniquely identified by a numeric `ChronicleId`, computed as `CityHash64(chronicleName)`. This deterministic hashing means that any process in the distributed system can independently derive the same ID from the same name without coordination.

Chronicles carry configurable attributes including indexing granularity (nanoseconds through seconds), type (standard or priority), and tiering policy (normal, hot, or cold). They also support user-defined properties and metadata, each capped at 16 entries, enabling application-specific annotations.

A single Chronicle can hold up to 1024 Stories and 1024 Archives. Story names are scoped within their parent Chronicle, so two different Chronicles may each contain a Story with the same name without conflict. The Chronicle maintains an acquisition reference count that tracks how many clients are currently using it.

## Story

A Story is a time-series dataset within a Chronicle. It represents a logical stream of events that client applications generate over time. Each Story is uniquely identified by a `StoryId`, computed as `CityHash64(chronicleName + storyName)`. Concatenating the Chronicle name ensures that identically named Stories in different Chronicles receive distinct IDs.

Stories have configurable attributes that control their behavior in the pipeline. The indexing granularity (nanosecond, microsecond, millisecond, or second) determines time resolution. The type (standard or priority) affects scheduling, and the tiering policy (normal, hot, or cold) influences storage placement decisions. An access permission field provides basic authorization control.

The ChronoVisor maintains an in-memory representation of each Story that tracks which clients have acquired (are actively writing to) the Story. This acquirer client map is protected by a mutex for thread-safe concurrent access. The Story's acquisition count acts as a reference counter — a Story cannot be removed while any client holds it.

## LogEvent

A LogEvent is the atomic data unit in ChronoLog. Every piece of data stored in the system is represented as a LogEvent. Each event carries five fields: the `storyId` identifying which Story it belongs to, an `eventTime` timestamp, the `clientId` of the application that generated it, an `eventIndex` (per-client counter to disambiguate events with identical timestamps), and a `logRecord` containing the opaque application payload.

Events are globally ordered by a composite key called `EventSequence`, which is the tuple `(eventTime, clientId, eventIndex)`. This three-part key is essential for distributed uniqueness: because multiple clients on different HPC nodes may generate events at the same timestamp, the `clientId` component distinguishes them. The `eventIndex` further handles the case where the same client produces multiple events within a single timestamp granularity (e.g., from different threads). Equality comparison checks all identifying fields (`storyId`, `eventTime`, `clientId`, `eventIndex`) but deliberately excludes the `logRecord` payload.

## StoryChunk

A StoryChunk is a container for LogEvents that fall within a specific time window `[startTime, endTime)` — including the start time and excluding the end time. Events within a StoryChunk are stored in a sorted map keyed by `EventSequence`, ensuring they are always maintained in global order.

StoryChunks are the unit of data movement through the distributed pipeline. ChronoKeeper processes on compute nodes collect events into StoryChunks during a configurable acceptance window. When the window expires, the StoryChunk is forwarded to a ChronoGrapher process on a storage node. Because multiple ChronoKeepers may handle events for the same Story, each produces a partial StoryChunk for overlapping time ranges. The ChronoGrapher merges these partial chunks into its own StoryPipeline — a sequence of StoryChunks ordered by start time.

Each StoryChunk also carries a `revisionTime` field that records when it was last modified, enabling the system to track data freshness during the merge process.

## Archive

An Archive provides historical storage associated with a Chronicle. Each Archive is identified by an `ArchiveId`, computed as `CityHash64(toString(chronicleId) + archiveName)`. Like Stories, Archive names are scoped within their parent Chronicle so that the same Archive name can be reused across different Chronicles. Archives carry a user-defined property list for application-specific metadata.

## Data Lifecycle

Events follow a well-defined path through the system. Client applications create LogEvents and send them to ChronoKeeper processes running on compute nodes. ChronoKeepers batch these events into StoryChunks during time-windowed acceptance periods. When the window closes, the StoryChunk is shipped to a ChronoGrapher process on a storage node, which merges partial chunks from different ChronoKeepers into a unified StoryPipeline. Finally, completed StoryChunks are extracted and persisted to the storage layer (HDF5 files). For a detailed walkthrough of this pipeline, see [Distributed Story Pipeline](./story-pipeline.md).

## Further Reading

| Topic | Page |
|---|---|
| Distributed pipeline design | [Distributed Story Pipeline](./story-pipeline.md) |
| C++ type definitions and APIs | [Core Types](./core-types.md) |
| HDF5 storage and serialization | [Storage and Serialization](./storage-and-serialization.md) |
| Service networking identities | [Service Identities](./service-identities.md) |
