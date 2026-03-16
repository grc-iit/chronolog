---
sidebar_position: 3
title: "ChronoKeeper"
---

# ChronoKeeper

**ChronoKeeper** is the component responsible for fast ingestion of the log events coming from the client processes, efficiently grouping them into partial StoryChunks and moving them down to the lower tier of ChronoLog system.

Most ChronoLog deployments would have ChronoKeeper processes installed on the majority of the compute nodes and each Recording Group is expected to have multiple ChronoKeeper processes.

![ChronoKeeper Internal Diagram](/diagrams/chrono-keeper-internal-diagram.jpg)

| Component | Description |
|-----------|-------------|
| **Keeper Data Store** | The main ChronoKeeper module. A collection of Story Pipelines for all actively recorded stories in the Recording Group. Instantiates a Story Pipeline when ChronoKeeper receives a StartStoryRecording notification and dismantles it after StopStoryRecording. Sequencing threads are responsible for ordering ingested log events and grouping them into time-range bound StoryChunks. (See ChronoLog Story Pipeline Data Model for reference.) |
| **Keeper Recording Service** | Listens to incoming streams of log events from client applications and passes the events to the Ingestion Queue module for processing. |
| **Ingestion Queue** | A collection of IngestionHandles for actively recorded stories the ChronoKeeper is expecting events for. Receives log events from the Keeper Recording Service and attributes them to the appropriate Story Ingestion Handles. Story Ingestion Handle is part of the Story Pipeline object exposed to the Ingestion Queue at instantiation time. |
| **StoryChunk Extraction Queue** | A mutex-protected deque of StoryChunk pointers, serving as the communication boundary between the Keeper Data Store and StoryChunk Extractor modules. |
| **StoryChunk Extractors** | ChronoKeeper has two extractor types: **CSVFileStoryChunkExtractor** writes retired StoryChunks to a configurable local POSIX directory; **StoryChunkExtractorRDMA** drains retired StoryChunks to ChronoGrapher over an RDMA communication channel. Extractors can be chained if needed. |
| **KeeperRegistryClient** | The client side of RPC communication between the ChronoKeeper process and ChronoVisor's Recording Process Registry Service. Sends Register/Unregister and Heartbeat/Statistics messages to ChronoVisor. |
| **DataStoreAdminService** | Listens to Start/Stop Story recording notifications from ChronoVisor. These notifications trigger instantiation or dismantling of the appropriate StoryPipelines based on client data access requests. |
