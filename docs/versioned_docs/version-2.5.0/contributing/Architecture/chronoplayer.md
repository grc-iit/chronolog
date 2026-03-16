---
sidebar_position: 5
title: "ChronoPlayer"
---

# ChronoPlayer

**ChronoPlayer** is the component responsible for the "story reading" side of the ChronoLog services, as it reads back the recorded data and serves the client applications' queries into the recorded events.

The ChronoPlayer design is the logical continuation of the internal design of ChronoKeeper and ChronoGrapher servers, as it is also based on the Story Pipeline model.

![ChronoPlayer Internal Diagram](/diagrams/chrono-player-internal-diagram.jpg)

| Component | Description |
|-----------|-------------|
| **Player Registry Client** | The client side of RPC communication between the ChronoPlayer process and ChronoVisor's ProcessRegistryService. Used to send Register/Unregister and periodic Heartbeat/Statistics messages to ChronoVisor. |
| **Player Data Store Admin Service** | Listens to Start/Stop Story recording notifications from ChronoVisor. These notifications trigger instantiation or dismantling of the appropriate active StoryPipelines based on client data access requests. |
| **Player Recording Service** | The RPC service that receives partial StoryChunks from ChronoKeeper processes — the same partial story chunks sent to ChronoGrapher to be merged and committed to the Persistent Storage layer. |
| **Player Data Store** | Maintains the most recent segment of StoryPipeline for every active story, applying the same chunk merging logic as ChronoGrapher. This allows ChronoPlayer to have the most recent story events sorted, merged, and available for playback requests before they are available in the slower Persistent Storage layer. |
| **Playback Service** | Listens to Story replay queries from client applications and serves responses. Returns all sorted events for a specific story within the requested time range. The most recent events may come from the active Player Data Store; the majority of requests are satisfied from the Persistent Storage layer. |
| **Playback Response Transfer Agent** | The RPC client responsible for bulk transfer of the range query response event series back to the requesting client application. Instantiated by the Playback Service for each client query, using the client's response-receiving service credentials provided in the query request. |
| **Archive Reading Agent** | Reads Chronicle and Story data persisted in HDF5 Archives, extracts the subset of events relevant to the requested range query, and transforms them into ChronoLog StoryChunks for use by the Playback Service. |
