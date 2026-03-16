---
sidebar_position: 2
title: "Core Concepts"
---

# Core Concepts

This page introduces the minimum mental model you need to start working with ChronoLog. Each concept links to a deeper reference for when you're ready to go further.

## Data Model — Chronicles, Stories, and Events

ChronoLog organizes data in a three-level hierarchy:

```
Chronicle
└── Story
    └── Event
```

- A **Chronicle** is a top-level namespace that groups related data streams together.
- A **Story** is a time-ordered sequence of events within a Chronicle — think of it as a single log stream or sensor feed.
- An **Event** is the smallest unit of data: a timestamped log record attached to a Story.

:::tip Deep dive
See the [Data Model Overview](../user-guide/data-model/overview.md) for field-level details and ordering guarantees.
:::

## System Components

ChronoLog is a distributed system composed of four services:

| Component | Role |
|---|---|
| **ChronoVisor** | Manages client sessions, metadata, and service discovery |
| **ChronoKeeper** | Ingests events from clients at high throughput |
| **ChronoGrapher** | Merges and persists event data to long-term storage |
| **ChronoPlayer** | Serves historical replay queries |

The ChronoLog client library abstracts all inter-service communication — your application code talks to a single API, not to individual services.

:::tip Deep dive
See the [System Overview](../user-guide/architecture/system-overview.md) for architecture details, data flow, and deployment topology.
:::

## Record and Replay Workflow

A typical ChronoLog session follows these steps:

1. **Connect** to the ChronoLog cluster
2. **Create a Chronicle** (or use an existing one)
3. **Acquire a Story** within the Chronicle
4. **Log Events** to the Story
5. **Replay** the Story to read back events in time order
6. **Release** the Story when done writing
7. **Disconnect** from the cluster

```text title="Pseudocode"
client = chronolog.connect(server_uri)

client.create_chronicle("my_chronicle")
client.acquire_story("my_chronicle", "sensor_feed")

client.log_event("my_chronicle", "sensor_feed", "temperature=72.1")
client.log_event("my_chronicle", "sensor_feed", "temperature=73.4")

events = client.replay("my_chronicle", "sensor_feed", start, end)

client.release_story("my_chronicle", "sensor_feed")
client.disconnect()
```

:::tip Deep dive
See the [Client API](../client/overview.md) for the full API reference and language-specific guides.
:::

## Next Steps

- [**Quick Start**](quick-start.md) — install ChronoLog and run your first deployment
- [**System Overview**](../user-guide/architecture/system-overview.md) — understand the full architecture
- [**Data Model**](../user-guide/data-model/overview.md) — learn about event ordering and storage
- [**First Steps Tutorial**](../tutorials/native-linux/first-steps.md) — a hands-on walkthrough of recording and replaying events
