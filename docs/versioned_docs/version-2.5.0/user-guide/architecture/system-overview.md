---
sidebar_position: 1
title: "System Overview"
---

# System Overview

ChronoLog is a distributed, tiered log storage system designed for high-performance computing (HPC) environments. It captures, sequences, and archives streams of timestamped events — called **Stories** — without relying on a central sequencer. Each event carries a physical timestamp assigned at the source, and ChronoLog's pipeline progressively merges and orders these events as they move through storage tiers. The system is built on Thallium RPC with OFI transport and supports RDMA for high-throughput bulk data movement.

## Component Architecture

ChronoLog is composed of five main components, each running as an independent process:

| Component | Role | Typical deployment |
|-----------|------|--------------------|
| [**ChronoVisor**](./chronovisor.md) | Orchestrator — client portal, metadata directory, process registry, load balancing | One per deployment, on a dedicated node |
| [**ChronoKeeper**](./chronokeeper.md) | Fast event ingestion — receives log events from clients, groups them into partial StoryChunks | Many per deployment, on compute nodes |
| [**ChronoGrapher**](./chronographer.md) | Merge and archive — merges partial StoryChunks into complete ones, writes to HDF5 persistent storage | One per Recording Group, on a storage node |
| [**ChronoPlayer**](./chronoplayer.md) | Read-back — serves story replay queries from both in-memory data and HDF5 archives | One per Recording Group, on a storage node |
| **Client Library** | Application-facing API (`chronolog_client.h`) for connecting, creating Chronicles/Stories, recording events, and replaying data | Linked into client applications |

## Recording Groups

A **Recording Group** is a logical grouping of recording processes that work together to handle a subset of the story recording workload:

- Each group contains **multiple ChronoKeepers**, **one ChronoGrapher**, and **one ChronoPlayer**.
- ChronoVisor assigns newly acquired stories to a recording group using uniform random distribution for load balancing.
- All processes in a group register with ChronoVisor and send periodic heartbeat/statistics messages so that ChronoVisor can monitor group health and composition.
- A deployment can have multiple Recording Groups, allowing ChronoLog to scale horizontally by adding more groups.

## Data Flow

### Write path

```
Client Application
  │  LogEvent (storyId, timestamp, clientId, eventIndex, record)
  ▼
ChronoVisor (port 5555, provider 55)
  │  AcquireStory → assigns story to a Recording Group
  │  Notifies Keepers/Grapher/Player via DataStoreAdmin RPCs
  ▼
ChronoKeeper (port 5555)
  │  Ingestion Queue → Story Pipeline (in-memory)
  │  Events are grouped into time-range-bound partial StoryChunks
  │  Retired chunks enter the StoryChunk Extraction Queue
  ▼
ChronoGrapher (port 9999, provider 99 — RDMA drain)
  │  Receives partial StoryChunks from all Keepers in the group
  │  Merges partials into complete StoryChunks via its own Story Pipeline
  │  Archives complete chunks to persistent storage
  ▼
HDF5 Archives (POSIX filesystem on storage node)
```

### Read path

```
Client Application
  │  ReplayStory(storyId, startTime, endTime)
  ▼
ChronoVisor
  │  Routes query to ChronoPlayer in the appropriate Recording Group
  ▼
ChronoPlayer
  ├── Player Data Store (in-memory) — most recent merged chunks
  └── Archive Reading Agent — reads from HDF5 persistent storage
  │
  ▼
Playback Response Transfer Agent → bulk transfer back to client
```

The Player maintains an in-memory copy of the most recent story segments (the same chunks sent to ChronoGrapher), so recent events can be served before they are fully committed to the archive tier.

## Communication Model

ChronoLog uses [Thallium](https://mochi.readthedocs.io/en/latest/thallium.html) as its RPC framework, layered on top of Mercury and OFI (OpenFabrics Interfaces). The default transport protocol is `ofi+sockets`; for InfiniBand clusters, `ofi+verbs` enables native RDMA.

### Default service ports and provider IDs

| Service | Port | Provider ID |
|---------|------|-------------|
| Visor Client Portal | 5555 | 55 |
| Visor Keeper Registry | 8888 | 88 |
| Keeper Recording Service | 5555 | — |
| Keeper→Grapher Drain (RDMA) | 9999 | 99 |
| DataStore Admin Service | 4444 | 44 |

### Registration and heartbeat protocol

1. Each Keeper, Grapher, and Player process starts by sending a **Register** RPC to ChronoVisor's Recording Process Registry Service (port 8888).
2. After registration, processes send periodic **Heartbeat/Statistics** messages so ChronoVisor can track liveness and load.
3. ChronoVisor maintains **DataStoreAdminClient** connections to every registered process and uses them to push **StartStoryRecording** / **StopStoryRecording** notifications when clients acquire or release stories.

## Key Concepts

| Term | Definition |
|------|------------|
| **Chronicle** | A named collection of Stories. Carries metadata, indexing granularity, type (standard/priority), and a tiering policy. |
| **Story** | An individual, named log stream within a Chronicle. The unit of data acquisition — clients acquire and release stories. |
| **StoryChunk** | A time-range-bound container of log events for a single story. Defined by a start time and end time; events within are ordered by timestamp. |
| **LogEvent** | A single timestamped record: `{storyId, eventTime, clientId, eventIndex, logRecord}`. |
| **StoryPipeline** | The processing pipeline inside Keepers, Graphers, and Players that ingests events/chunks, orders them by time, groups them into StoryChunks, and retires completed chunks to the next tier. |
| **Recording Group** | A set of Keeper + Grapher + Player processes that collectively handle story recording for a subset of the workload. |

For detailed data structure definitions, see the [Data Model](../data-model/overview.md) section.

## Tiered Storage Design

ChronoLog implements a three-tier storage hierarchy that progressively trades latency for capacity:

| Tier | Location | Component | Medium | Purpose |
|------|----------|-----------|--------|---------|
| **Hot** | Compute nodes | ChronoKeeper | In-memory (Story Pipeline) | Fast event ingestion with sub-second latency |
| **Warm** | Storage node | ChronoGrapher / ChronoPlayer | In-memory (Story Pipeline) | Chunk merging, recent-data playback |
| **Cold** | Storage node | ChronoGrapher (archiver) | HDF5 files on POSIX filesystem | Long-term persistent archive |

Data moves automatically from hot to cold:
- **Keepers** retire partial StoryChunks once they exceed the configured chunk duration (default: 30 seconds) or the story stops recording.
- **Graphers** merge partials from all Keepers into complete StoryChunks and archive them to HDF5.
- **Players** maintain a warm copy of recent chunks for fast read-back while the archive catches up.

Tiering policy can be set per-Chronicle (`normal`, `hot`, or `cold`) to bias toward performance or capacity.

## Design Principles

- **Physical timestamps** — Events carry timestamps assigned at the source. There is no global sequencer; ordering is resolved progressively through the pipeline.
- **Double-buffering** — StoryPipelines use a two-deque pattern (active and passive queues) so that ingestion and extraction can proceed in parallel without blocking each other. The active deque receives new data while the passive deque is drained by sequencing/extraction threads; they swap atomically when conditions are met.
- **Parallelized ingestion** — Multiple ChronoKeepers per Recording Group accept events concurrently, distributing ingestion load across compute nodes.
- **Batch data movement** — Retired StoryChunks are transferred in bulk from Keepers to Graphers, amortizing RPC overhead.
- **RDMA-capable transport** — The Keeper→Grapher drain path uses Thallium's `tl::bulk` for zero-copy RDMA transfers when the OFI provider supports it (`ofi+verbs`), falling back to `ofi+sockets` otherwise.
- **Single-writer per tier** — Each Recording Group has exactly one Grapher and one Player, avoiding write conflicts at the merge and archive stages.

## Further Reading

- **Component deep-dives:** [ChronoVisor](./chronovisor.md) | [ChronoKeeper](./chronokeeper.md) | [ChronoGrapher](./chronographer.md) | [ChronoPlayer](./chronoplayer.md)
- **Data Model:** [Overview](../data-model/overview.md) | [Core Types](../data-model/core-types.md) | [Storage and Serialization](../data-model/storage-and-serialization.md)
