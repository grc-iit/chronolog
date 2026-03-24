---
sidebar_position: 1
title: "Overview"
---

# Data Model Overview

ChronoLog's data model organizes time-series log events from distributed HPC applications into a hierarchical structure. Events flow from client processes through a multi-tier distributed pipeline, ultimately reaching persistent storage. The model is designed to support high-throughput ingestion from many concurrent clients while preserving global event ordering.

## Key Concepts

- **[Chronicle](./concepts.md#chronicle)** — A named top-level container that groups related Stories. Identified by `CityHash64(chronicleName)`. Supports configurable attributes and can hold up to 1024 Stories and 1024 Archives.
- **[Story](./concepts.md#story)** — A time-series dataset within a Chronicle representing a logical event stream. Carries configurable attributes for indexing granularity, type, tiering policy, and access control.
- **[LogEvent](./concepts.md#logevent)** — The atomic data unit, carrying a timestamp, client ID, event index, and an opaque payload. Globally ordered by `EventSequence = (eventTime, clientId, eventIndex)`.
- **[StoryChunk](./concepts.md#storychunk)** — A time-windowed batch of LogEvents `[startTime, endTime)`. The unit of data movement through the distributed pipeline, merged from partial chunks produced by multiple ChronoKeepers.
- **[Archive](./concepts.md#archive)** — Historical storage associated with a Chronicle, with user-defined metadata properties.

For detailed definitions and configuration options, see [Concepts](./concepts.md).

## Data Lifecycle

Events follow a well-defined path through the system. Client applications create LogEvents and send them to ChronoKeeper processes running on compute nodes. ChronoKeepers batch these events into StoryChunks during time-windowed acceptance periods. When the window closes, the StoryChunk is shipped to a ChronoGrapher process on a storage node, which merges partial chunks from different ChronoKeepers into a unified StoryPipeline. Finally, completed StoryChunks are extracted and persisted to the storage layer (HDF5 files). For a detailed walkthrough of this pipeline, see [Distributed Story Pipeline](./story-pipeline.md).

## Further Reading

| Topic | Page |
|---|---|
| Concept definitions and details | [Concepts](./concepts.md) |
| Distributed pipeline design | [Distributed Story Pipeline](./story-pipeline.md) |
| C++ type definitions and APIs | [Core Types](./core-types.md) |
| HDF5 storage and serialization | [Storage and Serialization](./storage-and-serialization.md) |
| Service networking identities | [Service Identities](./service-identities.md) |
