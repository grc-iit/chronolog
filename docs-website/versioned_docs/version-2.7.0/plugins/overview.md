---
sidebar_position: 1
title: "Overview"
---

# Plugins Overview

ChronoLog's core API is intentionally low-level: clients write opaque log records into Stories and replay them later. This design gives the storage layer maximum flexibility and performance, but it places the burden of interpretation squarely on the application. Plugins bridge that gap.

A **plugin** is a thin native library that sits between your application and the ChronoLog client library. It encodes a domain-specific interface — a key-value store, a stream consumer, a metric sink — on top of ChronoLog's primitive Chronicle/Story/Event model. The underlying data is still stored and replicated by ChronoLog exactly as it would be without the plugin; the plugin only defines how records are serialized into events and how those events are presented back to the caller.

## Why plugins?

Working directly with the ChronoLog API requires applications to manage their own serialization, timestamping conventions, and record layout. For many use cases this is unnecessary boilerplate. Plugins package those conventions once and expose a familiar, domain-specific interface so that:

- **Existing codebases can adopt ChronoLog with minimal changes.** A service already using a key-value interface can switch to ChronoKVS and immediately gain full version history without rewriting its data access layer.
- **Common query patterns are handled for you.** Plugins expose operations such as point-in-time lookups, range scans, and history reconstruction — patterns that are repetitive to implement correctly against the raw API.
- **Domain semantics are preserved.** Each plugin enforces a consistent record schema and event ordering contract, preventing accidental misuse of the underlying storage.

## How plugins work

Every plugin follows the same layered structure:

```
Your Application
      │
      ▼
  Plugin API          ← domain-specific interface (e.g. put/get for KVS)
      │
      ▼
ChronoLog Client      ← LogEvent / ReplayStory primitives
      │
      ▼
ChronoLog Cluster     ← Keepers, Graphers, Players, persistent storage
```

When you call a plugin operation, it serializes the call arguments into a ChronoLog log record, selects (or creates) the appropriate Chronicle and Story, and forwards the event to the cluster. On reads, the plugin retrieves raw events from ChronoLog and deserializes them back into the domain objects your application expects. No custom storage backend is needed — plugins inherit all of ChronoLog's durability, ordering, and scalability guarantees automatically.

## Available plugins

| Plugin | Interface | Primary use case |
|---|---|---|
| [ChronoKVS](./chronokvs.md) | Multi-version key-value store | State auditing, configuration history, temporal lookups |
| [ChronoStream](./chronostream.md) | Event stream consumer | Real-time processing, ordered delivery, stream replay |
| [ChronoViz](./chronoviz.md) | Grafana data source | Dashboard visualization, interactive exploration, time-range queries |

## Choosing a plugin

Use **ChronoKVS** when your application works with named key-value pairs and you need to query past values, reconstruct state at a specific point in time, or audit how a value has changed. It is a good fit for configuration stores, metadata registries, or any system where "what was the value of X at time T?" is a meaningful question.

Use **ChronoStream** when your application produces or consumes a continuous flow of events and you need ordered, at-least-once delivery with the ability to replay from any point in the stream. It is a good fit for telemetry pipelines, event-driven architectures, and distributed tracing backends.

Use **ChronoViz** when you want to explore and visualize ChronoLog data interactively through Grafana dashboards. It connects Grafana directly to a running ChronoLog service, letting you browse chronicles and stories, query events over arbitrary time ranges, and render the results in any Grafana panel without writing custom export scripts.

If neither plugin fits your use case, the raw ChronoLog [C++ client API](../client/cpp/overview.md) is always available and provides full control over record layout and storage semantics.
