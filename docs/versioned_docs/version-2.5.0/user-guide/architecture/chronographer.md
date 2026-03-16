---
sidebar_position: 4
title: "ChronoGrapher"
---

# ChronoGrapher

**ChronoGrapher** is the component that continues the job of sequencing and merging of the partial StoryChunks created by the ChronoKeepers on the first tier, assembling the complete time-range bound StoryChunks out of them, and archiving the complete StoryChunks into the persistent storage tier.

The ChronoGrapher design follows the StoryPipeline Data Model and mirrors the design of the ChronoKeeper. The ChronoLog deployment allows for only one ChronoGrapher process within the RecordingGroup, and it's usually deployed on the storage node.

![ChronoGrapher Internal Diagram](/diagrams/chrono-grapher-internal-diagram.jpg)

| Component | Description |
|-----------|-------------|
| **Recording Process Registry Client** | The client side of RPC communication between the ChronoGrapher process and ChronoVisor's RecordingProcessRegistryService. Used to send Register/Unregister and periodic Heartbeat/Statistics messages to ChronoVisor. |
| **DataStore Admin Service** | Listens to Start/Stop Story recording notifications from ChronoVisor. These notifications trigger instantiation or dismantling of the appropriate StoryPipelines based on client data access requests. |
| **Grapher Recording Service** | The RPC service that receives partial StoryChunks from ChronoKeeper processes using Thallium RDMA communication. |
| **StoryChunk Ingestion Queue** | Ensures thread-safe communication between the ingestion threads of the Recording Service and the sequencing threads of the Grapher Data Store. Each active StoryPipeline has a pair of active and passive StoryChunkIngestionHandles to manage StoryChunk processing efficiently. |
| **Grapher Data Store** | Maintains a StoryPipeline for each active story, merging ingested partial StoryChunks into complete time-range bound StoryChunks and retiring the complete StoryChunks to the Story Chunk Extraction Queue. |
| **StoryChunk Extraction Queue** | Provides a thread-safe communication boundary between the Grapher Data Store and the Extractor modules as retired StoryChunks leave their StoryPipelines and wait to be picked up by the extractor. |
| **StoryChunk Archiving Extractor** | Handles the serialization and archiving of StoryChunks from the Story Chunk Extraction Queue to persistent storage. |
