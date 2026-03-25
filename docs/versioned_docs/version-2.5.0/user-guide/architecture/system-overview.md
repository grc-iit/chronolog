---
sidebar_position: 1
title: "System Overview"
---

# System Overview

ChronoLog is a distributed, tiered log storage system designed for High-Performance Computing (HPC) environments. It captures, sequences, and archives streams of timestamped **Events** — called **Stories** — without relying on a central sequencer. Each **Event** carries a physical timestamp assigned at the source, and ChronoLog's pipeline progressively merges and orders these **Events** as they move through storage tiers. The system is built on Thallium RPC with OFI transport and supports RDMA for high-throughput bulk data movement.

## Component Architecture

ChronoLog is composed of five main components, each running as an independent process:

| Component                                      | Role                                                                                                                                    | Typical deployment                             |
| ------------------------------------------------| -----------------------------------------------------------------------------------------------------------------------------------------| ------------------------------------------------|
| [**ChronoVisor**](./chronovisor.md)            | Orchestrator — client portal, metadata directory, process registry, load balancing                                                      | One per deployment, on a dedicated node        |
| [**ChronoKeeper**](./chronokeeper.md)          | Fast event ingestion — receives log events from clients, groups them into partial **StoryChunks**                                       | Many per deployment, on compute nodes          |
| [**ChronoGrapher**](./chronographer.md)        | Merge and archive — merges partial **StoryChunks** into complete ones, writes to persistent storage as HDF5 files                       | One per **Recording Group**, on a storage node |
| [**ChronoPlayer**](./chronoplayer.md)          | Read-back — serves story replay queries from both in-memory data and HDF5 archives                                                      | One per **Recording Group**, on a storage node |
| [**Client Library**](../../client/overview.md) | Application-facing API (`chronolog_client.h`) for connecting, creating **Chronicles**/**Stories**, recording events, and replaying data | Linked into client applications                |

## Recording Groups

A **Recording Group** is a logical grouping of recording processes that work together to handle a subset of the story recording workload:

- Each group contains **multiple ChronoKeepers**, **one ChronoGrapher**, and **one ChronoPlayer**.
- ChronoVisor assigns newly acquired **Stories** to a recording group using uniform random distribution for load balancing.
- All processes in a group register with ChronoVisor and send periodic heartbeat/statistics messages so that ChronoVisor can monitor group health and composition.
- A deployment can have multiple Recording Groups, allowing ChronoLog to scale horizontally by adding more groups.

## Data Flow

### Write path

<svg viewBox="0 0 800 130" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="800" height="130" rx="10" fill="#1e2330"/>

  {/* Title */}
  <text x="400" y="20" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">WRITE PATH</text>

  {/* Client Application */}
  <rect x="16" y="36" width="108" height="80" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="32" cy="52" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="40" y="56" fill="#c3e04d" fontSize="8" fontWeight="600">Client App</text>
  <text x="24" y="72" fill="#9ca3b0" fontSize="6">log_event(record)</text>

  {/* Arrow — App → Client Library */}
  <line x1="124" y1="76" x2="136" y2="76" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="138,76 132,72 132,80" fill="#c3e04d" fillOpacity="0.5"/>

  {/* Client Library */}
  <rect x="142" y="36" width="98" height="80" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="158" cy="52" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="166" y="56" fill="#c3e04d" fontSize="8" fontWeight="600">Client Library</text>
  <text x="150" y="81" fill="#9ca3b0" fontSize="6">Timestamps record,</text>
  <text x="150" y="90" fill="#9ca3b0" fontSize="6">calls RPC to send</text>
  <text x="150" y="99" fill="#9ca3b0" fontSize="6">event to ChronoVisor</text>

  {/* Arrow — Client Library → ChronoVisor */}
  <line x1="240" y1="76" x2="252" y2="76" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="254,76 248,72 248,80" fill="#c3e04d" fillOpacity="0.5"/>

  {/* ChronoVisor */}
  <rect x="258" y="36" width="115" height="80" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="274" cy="52" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="282" y="56" fill="#c3e04d" fontSize="8" fontWeight="600">ChronoVisor</text>
  <text x="268" y="72" fill="#9ca3b0" fontSize="6">{"AcquireStory → assigns"}</text>
  <text x="268" y="81" fill="#9ca3b0" fontSize="6">story to Recording Group.</text>
  <text x="268" y="90" fill="#9ca3b0" fontSize="6">Notifies Keepers/Grapher</text>
  <text x="268" y="99" fill="#9ca3b0" fontSize="6">via DataStoreAdmin RPCs</text>

  {/* Arrow — ChronoVisor → ChronoKeeper */}
  <line x1="373" y1="76" x2="385" y2="76" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="387,76 381,72 381,80" fill="#c3e04d" fillOpacity="0.5"/>

  {/* ChronoKeeper */}
  <rect x="393" y="36" width="125" height="80" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="409" cy="52" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="417" y="56" fill="#c3e04d" fontSize="8" fontWeight="600">ChronoKeeper</text>
  <text x="403" y="72" fill="#9ca3b0" fontSize="6">{"Ingestion Queue → Story"}</text>
  <text x="403" y="81" fill="#9ca3b0" fontSize="6">Pipeline (in-memory). Events</text>
  <text x="403" y="90" fill="#9ca3b0" fontSize="6">grouped into partial StoryChunks.</text>
  <text x="403" y="99" fill="#9ca3b0" fontSize="6">Retired chunks extracted</text>

  {/* Arrow — ChronoKeeper → ChronoGrapher */}
  <line x1="518" y1="76" x2="530" y2="76" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="532,76 526,72 526,80" fill="#c3e04d" fillOpacity="0.5"/>

  {/* ChronoGrapher */}
  <rect x="540" y="36" width="125" height="80" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="556" cy="52" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="564" y="56" fill="#c3e04d" fontSize="8" fontWeight="600">ChronoGrapher</text>
  <text x="550" y="72" fill="#9ca3b0" fontSize="6">Receives partial StoryChunks</text>
  <text x="550" y="81" fill="#9ca3b0" fontSize="6">from all Keepers. Merges into</text>
  <text x="550" y="90" fill="#9ca3b0" fontSize="6">complete chunks. Archives to</text>
  <text x="550" y="99" fill="#9ca3b0" fontSize="6">persistent storage</text>

  {/* Arrow — ChronoGrapher → HDF5 */}
  <line x1="665" y1="76" x2="677" y2="76" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="679,76 673,72 673,80" fill="#c3e04d" fillOpacity="0.5"/>

  {/* HDF5 Archives */}
  <rect x="685" y="36" width="99" height="80" rx="6" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <circle cx="701" cy="52" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="709" y="56" fill="#e4e7ed" fontSize="8" fontWeight="600">HDF5 Archives</text>
  <text x="695" y="72" fill="#9ca3b0" fontSize="6">POSIX filesystem</text>
  <text x="695" y="81" fill="#9ca3b0" fontSize="6">on storage node</text>

</svg>

1. Client app calls `log_event()` with payload → passes to Client library
2. Client library timestamps the Event → sends to ChronoVisor
3. ChronoVisor assigns the Story to a Recording Group → notifies all group processes
4. ChronoKeeper ingests Events into in-memory Story Pipeline → groups into partial StoryChunks
5. Retired chunks are drained via RDMA bulk transfer to ChronoGrapher
6. ChronoGrapher merges partials from all Keepers → archives complete StoryChunks to HDF5 archive files

### Read path

<svg viewBox="0 0 720 470" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="470" rx="10" fill="#1e2330"/>

  {/* Title */}
  <text x="360" y="20" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">READ PATH</text>

  {/* Client Application */}
  <rect x="110" y="36" width="500" height="44" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="126" cy="54" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="134" y="58" fill="#c3e04d" fontSize="10" fontWeight="600">Client Application</text>
  <text x="126" y="72" fill="#9ca3b0" fontSize="7">ReplayStory(storyId, startTime, endTime)</text>

  {/* Arrow 1 */}
  <line x1="360" y1="80" x2="360" y2="114" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="360,116 356,110 364,110" fill="#c3e04d" fillOpacity="0.5"/>

  {/* ChronoVisor */}
  <rect x="110" y="118" width="500" height="48" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="126" cy="136" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="134" y="140" fill="#c3e04d" fontSize="10" fontWeight="600">ChronoVisor</text>
  <text x="126" y="156" fill="#9ca3b0" fontSize="7">Routes query to ChronoPlayer in the appropriate Recording Group</text>

  {/* Arrow 2 */}
  <line x1="360" y1="166" x2="360" y2="198" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="360,200 356,194 364,194" fill="#c3e04d" fillOpacity="0.5"/>

  {/* ChronoPlayer container (dashed) */}
  <rect x="50" y="192" width="620" height="178" rx="8" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
  <text x="62" y="206" fill="#4a90a4" fontSize="8" fontWeight="600" fillOpacity="0.7">CHRONOPLAYER</text>

  {/* ChronoPlayer box */}
  <rect x="160" y="212" width="400" height="38" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="176" cy="232" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="184" y="236" fill="#c3e04d" fontSize="10" fontWeight="600">ChronoPlayer</text>
  <text x="340" y="236" fill="#9ca3b0" fontSize="7">Queries both in-memory and archival data sources</text>

  {/* Fork arrow left */}
  <line x1="280" y1="250" x2="205" y2="296" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="203,298 200,291 208,292" fill="#c3e04d" fillOpacity="0.5"/>

  {/* Fork arrow right */}
  <line x1="440" y1="250" x2="515" y2="296" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="517,298 512,292 520,291" fill="#c3e04d" fillOpacity="0.5"/>

  {/* Player Data Store (left branch) */}
  <rect x="70" y="300" width="270" height="48" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="86" cy="318" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="94" y="322" fill="#c3e04d" fontSize="9" fontWeight="600">Player Data Store (in-memory)</text>
  <text x="86" y="338" fill="#9ca3b0" fontSize="7">Most recent merged chunks</text>

  {/* Archive Reading Agent (right branch) */}
  <rect x="380" y="300" width="270" height="48" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="396" cy="318" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="404" y="322" fill="#c3e04d" fontSize="9" fontWeight="600">Archive Reading Agent</text>
  <text x="396" y="338" fill="#9ca3b0" fontSize="7">Reads from HDF5 persistent storage</text>

  {/* Converge arrow left */}
  <line x1="205" y1="348" x2="355" y2="404" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>

  {/* Converge arrow right */}
  <line x1="515" y1="348" x2="365" y2="404" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>

  {/* Converge arrowhead */}
  <polygon points="360,406 356,400 364,400" fill="#c3e04d" fillOpacity="0.5"/>

  {/* Playback Response Transfer Agent */}
  <rect x="110" y="408" width="500" height="44" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="126" cy="426" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="134" y="430" fill="#c3e04d" fontSize="10" fontWeight="600">Playback Response Transfer Agent</text>
  <text x="126" y="444" fill="#9ca3b0" fontSize="7">{"Bulk transfer back to client → merged, time-ordered event stream"}</text>
</svg>

The Player maintains an in-memory copy of the most recent story segments (the same chunks sent to ChronoGrapher), so recent events can be served before they are fully committed to the archive tier.

## Communication Model

ChronoLog uses [Thallium](https://mochi.readthedocs.io/en/latest/thallium.html) as its RPC framework, layered on top of Mercury and OFI (OpenFabrics Interfaces). The default transport protocol is `ofi+sockets`; for clusters with RDMA support, `ofi+verbs` enables native RDMA.

### Default service ports and provider IDs

| Service                     | Port | Provider ID |
| -----------------------------| ------| -------------|
| Visor Client Portal         | 5555 | 55          |
| Visor Keeper Registry       | 8888 | 88          |
| Keeper Recording Service    | 6666 | 66          |
| Keeper→Grapher Drain (RDMA) | 9999 | 99          |
| DataStore Admin Service     | 4444 | 44          |

### Registration and heartbeat protocol

1. Each Keeper, Grapher, and Player process starts by sending a **Register** RPC to ChronoVisor's Recording Process Registry Service (port 8888).
2. After registration, processes send periodic **Heartbeat/Statistics** messages so ChronoVisor can track liveness and load.
3. ChronoVisor maintains **DataStoreAdminClient** connections to every registered process and uses them to push **StartStoryRecording** / **StopStoryRecording** notifications when clients acquire or release stories.

## Key Concepts

| Term                | Definition                                                                                                                                                                                  |
| ---------------------| ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Chronicle**       | A named collection of Stories. Carries metadata, indexing granularity, type (standard/priority), and a tiering policy.                                                                      |
| **Story**           | An individual, named log stream within a Chronicle. The unit of data acquisition — clients acquire and release stories.                                                                     |
| **StoryChunk**      | A time-range-bound container of log events for a single story. Defined by a start time and end time; events within are ordered by timestamp.                                                |
| **LogEvent**        | A single timestamped record: `{storyId, eventTime, clientId, eventIndex, logRecord}`.                                                                                                       |
| **StoryPipeline**   | The processing pipeline inside Keepers, Graphers, and Players that ingests events/chunks, orders them by time, groups them into StoryChunks, and retires completed chunks to the next tier. |
| **Recording Group** | A set of Keeper + Grapher + Player processes that collectively handle story recording for a subset of the workload.                                                                         |

For detailed data structure definitions, see the [Data Model](../data-model/overview.md) section.

## Tiered Storage Design

ChronoLog implements a three-tier storage hierarchy that progressively trades latency for capacity:

| Tier     | Location      | Component                    | Medium                         | Purpose                                      |
| ----------| ---------------| ------------------------------| --------------------------------| ----------------------------------------------|
| **Hot**  | Compute nodes | ChronoKeeper                 | In-memory (Story Pipeline)     | Fast event ingestion with sub-second latency |
| **Warm** | Storage node  | ChronoGrapher / ChronoPlayer | In-memory (Story Pipeline)     | Chunk merging, recent-data playback          |
| **Cold** | Storage node  | ChronoGrapher                | HDF5 files on POSIX filesystem | Long-term persistent archive                 |

Data moves automatically from hot to cold:
- **Keepers** retire partial StoryChunks once they exceed the configured chunk duration (default: 30 seconds) or the story stops recording.
- **Graphers** merge partials from all Keepers into complete StoryChunks and archive them to HDF5 files.
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
