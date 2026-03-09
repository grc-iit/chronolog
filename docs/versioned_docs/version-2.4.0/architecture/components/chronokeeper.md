---
sidebar_position: 2
title: "ChronoKeeper"
---

# ChronoKeeper

**ChronoKeeper** is the component responsible for fast ingestion of the log events coming from the client processes, efficiently grouping them into partial StoryChunks and moving them down to the lower tier of ChronoLog system.

Most ChronoLog deployments would have ChronoKeeper processes installed on the majority of the compute nodes and each Recording Group is expected to have multiple ChronoKeeper processes.

![ChronoKeeper](/img/ChronoKeeper.jpg)

- **Keeper Data Store is the main ChronoKeeper module**. Keeper Data Store is the collection of Story Pipelines for all the actively recorded stories for the Recording Group that this ChronoKeeper process is part of. Keeper Data Store instantiates the Story Pipeline for the specific Story when the ChronoKeeper receives StartStoryRecording notification for this Story and dismantles the Story Pipeline shortly after being notified to StopStoryRecording. The Keeper Data Store sequencing threads are responsible for sequencing the ingested log events and grouping them into the time-range bound StoryChunks. (See ChronoLog Story Pipeline Data Model for reference)
- **Keeper Recording Service** listens to the incoming streams of log events from the client applications and passes the events to the Ingestion Queue module for processing.
- **Ingestion Queue** module is a collection of IngestionHandles for actively recorded stories the ChronoKeeper is expecting the events for. Ingestion Queue gets the log events from the Keeper Recording Service and attributes them to the appropriate Story Ingestion Handles. Story Ingestion Handle is part of the Story Pipeline object that is exposed to the Ingestion Queue at the time of Story Pipeline instantiation.
- **StoryChunk Extraction Queue** is a mutex-protected deque of StoryChunk pointers, serving as the communication boundary between Keeper Data Store and StoryChunk Extractor modules.
- **StoryChunk Extractors** ChronoKeeper has two types of StoryChunk Extractors currently implemented: **CSVFileStoryChunkExtractor** writes retired StoryChunks to a configurable local POSIX directory. **StoryChunkExtractorRDMA** drains retired StoryChunks to the ChronoGrapher over an RDMA communication channel. The StoryChunk Extractors can be chained if needed.
- **KeeperRegistryClient** is the client side of RPC communication between the ChronoKeeper process and ChronoVisor's Recording Process Registry Service. KeeperRegistryClient sends out Register/Unregister and Heartbeat/Statistics messages to the ChronoVisor.
- **DataStoreAdminService** is the service listening to the Start/Stop Story recording notifications from the ChronoVisor, these notifications trigger instantiation or dismantling of the appropriate StoryPipelines based on the clients data access requests.
