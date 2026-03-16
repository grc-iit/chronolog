---
sidebar_position: 2
title: "ChronoVisor"
---

# ChronoVisor

**ChronoVisor is the orchestrating component of the ChronoLog system**. There is only one ChronoVisor process in a ChronoLog deployment, typically installed on its own node.

ChronoVisor acts as the client-facing portal, managing client connections and data access requests. It also maintains the registry of active recording processes and distributes the load of data recording across the recording process groups.

![ChronoVisor Internal Diagram](/diagrams/chrono-visor-internal-diagram.png)


| Component | Description |
|-----------|-------------|
| **Client Portal RPC Service** | Manages communication sessions between client applications and ChronoVisor using a Thallium RPC engine configured for efficient concurrent connections. |
| **Client Authentication Module** | Defines the authentication and authorization interface for client access control. In v2.5.0 all checks are implemented as stubs that grant access unconditionally; full role-based authentication is planned for a future release. |
| **Client Registry** | Keeps track of all active client connections and clients' data acquisition requests for Chronicles and Stories. |
| **RecordingProcessRegistryService** | Listens to RPC Registration and Heartbeat/Statistics messages coming from ChronoKeeper, ChronoGrapher, and ChronoPlayer processes and passes these messages to the Recording Group Registry to act upon. |
| **Recording Group Registry** | Monitors and manages the *Recording Groups* process composition and activity status. ChronoVisor uses this information to distribute the load of data recording by assigning each newly acquired story to a specific recording group using a uniform random distribution. Recording Group Registry maintains **DataStoreAdminClient** connections to all registered ChronoKeepers, ChronoGraphers, and ChronoPlayers and uses them to issue RPC notifications about the start and stop of story recording to all recording processes involved. |
| **Chronicle and Story Meta Directory** | Manages the Chronicle and Story metadata for the ChronoLog system, coordinating with Client Registry to track client data acquisition sessions for specific Chronicles and Stories, and with the Recording Group Registry for distributing data access tasks between the recording process groups. |
