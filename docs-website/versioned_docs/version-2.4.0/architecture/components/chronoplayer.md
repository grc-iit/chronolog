---
sidebar_position: 4
title: "ChronoPlayer"
---

# ChronoPlayer

**ChronoPlayer** is the component responsible for the "story reading" side of the ChronoLog services, as it reads back the recorded data and serves the client applications' queries into the recorded events.

The ChronoPlayer design is the logical continuation of the internal design of ChronoKeeper and ChronoGrapher servers, as it is also based on the Story Pipeline model.

![ChronoPlayer](/component-icons/chrono-player-plain.svg)

- **Player Registry Client** is the client side of RPC communication between the ChronoPlayer process and ChronoVisor's ProcessRegistryService. Player Registry Client is used to send Register/Unregister messages and periodic Heartbeat/Statistics messages to the ChronoVisor.
- **Player Data Store Admin Service** is the service listening to the Start/Stop Story recording notifications from the ChronoVisor, these notifications trigger instantiation or dismantling of the appropriate active StoryPipelines based on the clients data access requests.
- **Player Recording Service** in the RPC service that receives partial StoryChunks from ChronoKeeper processes, these are the same partial story chunks that are sent to the ChronoGrapher to be merged and committed to the Persistent Storage layer.
- **Player Data Store** maintains the most recent segment of StoryPipeline for every active story, applying the same story chunk merging logic of partial story chunks coming from the chronoKeepers as the ChronoGrapher does. This allows the ChronoPlayer to have the most recent story events sorted, merged, and available for serving playback requests before they are available in the slower Persistent Storage layer.
- **Playback Service** is the service listening to the Story replay queries from the client applications and serving the responses to these queries. Current implementation facilitates serving back all the sorted events for the specific story within the time range specified in the request. The most recent events might be coming from the active story Player Data Store though the majority of the requests would be satisfied with the data coming from the Persistent Storage layer.
- **Playback Response Transfer Agent** is the RPC communication client that is responsible for bulk transfer of the event series set that is the range query response back to the requesting client application. Transfer Agent is instantiated by the Playback Service for each client query using the client application query response receiving service credentials that are communicated in the query request.
- **Archive Reading Agent** is the module that is responsible for reading the Chronicle & Story data persisted to HDF5 Archives, extracting the subset of the events relevant to the range query requested, and transforming this subset into the set chronoLog Story Chunks that would be used in the query Response by the Playback Service.
