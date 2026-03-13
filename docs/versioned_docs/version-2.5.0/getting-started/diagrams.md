---
sidebar_position: 4
title: "Diagrams"
---

# Diagrams

This page tracks all architecture and documentation diagrams for ChronoLog v2.5.0. Diagrams are organized by phase and built incrementally. Each entry describes the diagram's purpose and current status.

---

## Phase 1 — Structure

> What exists: the components, their roles, and how they relate to each other.

| Name | Purpose | Description | Status |
|---|---|---|---|
| System Context | Entry point for new readers | ChronoLog as a black box — who uses it (HPC applications, operators) and what external systems it touches (parallel file system, external storage). No internals. | 🔲 todo |
| Container Overview | Main architecture reference | All deployable components (ChronoVisor, ChronoKeeper, ChronoGrapher, ChronoPlayer, storage), their roles, and connections. Includes Recording Group boundary and control vs. data plane distinction. | ✅ done — see [Core Concepts](./core-concepts.md) |
| ChronoVisor internals | Understand the control plane | Internal components: ClientPortalService, KeeperRegistry, ChronicleMetaDirectory, GlobalClock, and how they interact. | 🔲 todo |
| ChronoKeeper internals | Understand the ingestion pipeline | Internal components: KeeperRecordingService, IngestionQueue, StoryChunkPipeline, StoryChunkExtractor, and the event-to-chunk flow. | 🔲 todo |
| ChronoGrapher internals | Understand the persistence pipeline | Internal components: GrapherRecordingService, StoryChunkExtractionModule, HDF5FileChunkExtractor, StoryChunkWriter. | 🔲 todo |
| ChronoPlayer internals | Understand the replay pipeline | Internal components: PlaybackService, ArchiveReadingAgent, HDF5ArchiveReadingAgent, StoryChunkTransferAgent, and the read-back flow. | 🔲 todo |

---

## Phase 2 — Behavior

> How it works: interactions, sequences, and lifecycle state transitions.

| Name | Purpose | Description | Status |
|---|---|---|---|
| Startup & Registration | Operator reference for startup order | Sequence showing how ChronoKeeper, ChronoGrapher, and ChronoPlayer register with ChronoVisor before any client connects. Clarifies required startup order and what happens on process join/leave. | 🔲 todo |
| Connect / Disconnect | API reference | Sequence for `Connect()` and `Disconnect()` — client session lifecycle with ChronoVisor. | 🔲 todo |
| AcquireStory / ReleaseStory | API reference | Sequence for `AcquireStory()` — the service discovery flow: Visor selects a Recording Group, notifies Keepers and Grapher, returns addresses to client. Includes `ReleaseStory()` teardown. | 🔲 todo |
| log_event | API reference | Sequence for the write hot path — client round-robin selection of a Keeper, event ingestion, StoryChunk batching, and drain to ChronoGrapher. | 🔲 todo |
| ReplayStory | API reference | Sequence for `ReplayStory()` — client request to ChronoPlayer, archive read, RDMA push-back of StoryChunks to the client's `ClientQueryService`. | 🔲 todo |
| Story lifecycle | Concept reference | State diagram for a Story: `created → acquired → recording → released → destroyed`. Clarifies which API calls are valid in each state. | 🔲 todo |

---

## Phase 3 — Deployment

> Where it runs: node types, deployment topologies, and scaling.

| Name | Purpose | Description | Status |
|---|---|---|---|
| Single-node deployment | Dev / test reference | All components on one machine. Shows ports, config dependencies, and startup order for a local setup. | 🔲 todo |
| Multi-node deployment | Production reference | Which component runs on which node type — compute nodes (Keeper), storage nodes (Grapher, Player), management node (Visor). Shows network zones. | 🔲 todo |
| Scale-out | Scaling reference | N Recording Groups side by side, M Keepers per group. Shows how ChronoVisor assigns stories across groups and how clients hit different groups in parallel. | 🔲 todo |

---

## Phase 4 — Data

> How data moves and is stored over time. Planned for a future documentation phase.

| Name | Purpose | Description | Status |
|---|---|---|---|
| Data / chunk lifecycle | Deep-dive reference | LogEvent → StoryChunk → HDF5 file, with time dimension. Shows how chunks age out of Keepers and land in storage. | 🔲 planned |
| Storage format | Deep-dive reference | What an HDF5 archive looks like on disk — file naming convention, dataset structure, compound types. | 🔲 planned |
| Temporal query model | Deep-dive reference | How a time-range replay request maps to specific HDF5 files and datasets. | 🔲 planned |
