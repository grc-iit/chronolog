---
sidebar_position: 1
title: "ChronoVisor"
---

# ChronoVisor

**ChronoVisor is the orchestrating component of ChronoLog system**. There's only one ChronoVisor process in the ChronoLog system deployment, usually installed on its own node.

ChronoVisor acts as the client-facing portal managing the client connections and data access requests and it also maintains the registry of active recording processes and distributes the load of data recording between the recording process groups.

![ChronoVisor](/img/ChronoVisor.jpg)

- **Client Portal RPC Service** manages communication sessions between the client applications and ChronoVisor using a multithreaded engine for efficient concurrent connections.
- **Client Authentication Module** authenticates the clients before they are granted access to data. We are using the operating system's authentication in the 1st ChronoLog release. We plan to implement role-based authentication in later releases.
- **Client Registry** keeps track of all active client connections and clients' data acquisition requests for Chronicle and Stories.
- **RecordingProcessRegistryService** listens to RPC Registration and Heartbeat/Statistics messages coming from ChronoKeeper and ChronoGrapher processes and passes these messages to the Recording Group Registry to act upon.
- **Recording Group Registry** monitors and manages the *Recording Groups* process composition and activity status. ChronoVisor uses this information to distribute the load of data recording and management by assigning the newly acquired story to the specific recording group using uniform distribution algorithm. Recording Group Registry maintains **DataStoreAdminClient** connection to all the registered ChronoKeepers and ChronoGraphers and uses them to issue RPC notifications about the start and stop of the specific story recording to all the recording processes involved.
- **Chronicle and Story Meta Directory** manages the Chronicle and Story metadata for the ChronoLog system, coordinating with Client Registry to keep track of client data acquisition sessions for the specific Chronicles and Stories and with the RecordingGroup Registry for distributing data access tasks between the recording process groups.
