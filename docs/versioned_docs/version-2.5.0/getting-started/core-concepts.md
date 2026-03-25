---
sidebar_position: 2
title: "Core Concepts"
---

# Core Concepts

This page introduces the minimum mental model you need to start working with ChronoLog. Each concept links to a deeper reference for when you're ready to go further.

## Data Model — Chronicles, Stories, and Events

ChronoLog organizes data in a three-level hierarchy:

<svg viewBox="0 0 720 240" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="240" rx="10" fill="#1e2330"/>

  {/* Chronicle outer container */}
  <rect x="20" y="14" width="680" height="212" rx="8" fill="none" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.25" strokeDasharray="8,4"/>
  <circle cx="36" cy="32" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="44" y="36" fill="#c3e04d" fontSize="10" fontWeight="600">Chronicle</text>
  <text x="130" y="36" fill="#9ca3b0" fontSize="8">top-level namespace</text>

  {/* Story 1 */}
  <rect x="40" y="52" width="270" height="162" rx="6" fill="#252b3b" stroke="#4a90a4" strokeWidth="0.75" strokeOpacity="0.5"/>
  <circle cx="56" cy="70" r="3" fill="#4a90a4" fillOpacity="0.8"/>
  <text x="64" y="74" fill="#4a90a4" fontSize="9" fontWeight="600">Story</text>
  <text x="108" y="74" fill="#9ca3b0" fontSize="7">time-ordered event stream</text>

  {/* Events in Story 1 */}
  <rect x="52" y="86" width="246" height="24" rx="3" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <circle cx="66" cy="98" r="2" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="74" y="102" fill="#e4e7ed" fontSize="8">Event</text>
  <text x="112" y="102" fill="#9ca3b0" fontSize="7">timestamp + log record</text>

  <rect x="52" y="116" width="246" height="24" rx="3" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <circle cx="66" cy="128" r="2" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="74" y="132" fill="#e4e7ed" fontSize="8">Event</text>
  <text x="112" y="132" fill="#9ca3b0" fontSize="7">timestamp + log record</text>

  <rect x="52" y="146" width="246" height="24" rx="3" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <circle cx="66" cy="158" r="2" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="74" y="162" fill="#e4e7ed" fontSize="8">Event</text>
  <text x="112" y="162" fill="#9ca3b0" fontSize="7">timestamp + log record</text>

  <text x="175" y="196" textAnchor="middle" fill="#9ca3b0" fontSize="7" fontStyle="italic">ordered by time</text>

  {/* Ellipsis between stories */}
  <text x="350" y="140" textAnchor="middle" fill="#9ca3b0" fontSize="14">...</text>

  {/* Story 2 */}
  <rect x="390" y="52" width="270" height="162" rx="6" fill="#252b3b" stroke="#4a90a4" strokeWidth="0.75" strokeOpacity="0.5"/>
  <circle cx="406" cy="70" r="3" fill="#4a90a4" fillOpacity="0.8"/>
  <text x="414" y="74" fill="#4a90a4" fontSize="9" fontWeight="600">Story</text>
  <text x="458" y="74" fill="#9ca3b0" fontSize="7">time-ordered event stream</text>

  {/* Events in Story 2 */}
  <rect x="402" y="86" width="246" height="24" rx="3" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <circle cx="416" cy="98" r="2" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="424" y="102" fill="#e4e7ed" fontSize="8">Event</text>
  <text x="462" y="102" fill="#9ca3b0" fontSize="7">timestamp + log record</text>

  <rect x="402" y="116" width="246" height="24" rx="3" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <circle cx="416" cy="128" r="2" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="424" y="132" fill="#e4e7ed" fontSize="8">Event</text>
  <text x="462" y="132" fill="#9ca3b0" fontSize="7">timestamp + log record</text>

  <rect x="402" y="146" width="246" height="24" rx="3" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <circle cx="416" cy="158" r="2" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="424" y="162" fill="#e4e7ed" fontSize="8">Event</text>
  <text x="462" y="162" fill="#9ca3b0" fontSize="7">timestamp + log record</text>

  <text x="525" y="196" textAnchor="middle" fill="#9ca3b0" fontSize="7" fontStyle="italic">ordered by time</text>
</svg>

- A **Chronicle** is a top-level collection that groups Stories together.
- A **Story** is a time-ordered sequence of Events — think of it as a single log stream or sensor feed.
- An **Event** is the smallest unit of data: a timestamped log record attached to a Story.

*For field-level details and ordering guarantees, see the [Data Model Overview](../user-guide/data-model/overview.md).*

## System Components

ChronoLog is a distributed system composed of four services:

| Component         | Role                                                     |
| -------------------| ----------------------------------------------------------|
| **ChronoVisor**   | Manages client sessions, metadata, and service discovery |
| **ChronoKeeper**  | Ingests Events from clients at high throughput           |
| **ChronoGrapher** | Merges and persists Event data to long-term storage      |
| **ChronoPlayer**  | Serves historical replay queries                         |

The ChronoLog client library abstracts all inter-service communication — your application code talks to a single API, not to individual services.

*For architecture details, data flow, and deployment topology, see the [System Overview](../user-guide/architecture/system-overview.md).*

## Record and Replay Workflow

A typical ChronoLog session follows these steps:

1. **Connect** to the ChronoLog cluster
2. **Create a Chronicle** (or use an existing one)
3. **Acquire a Story** within the Chronicle
4. **Log Events** to the Story
5. **Replay** the Story to read back Events in time order
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

*For the full API reference and language-specific guides, see the [Client API](../client/overview.md).*

## Next Steps

- [**Quick Start**](quick-start.md) — install ChronoLog and run your first deployment
- [**System Overview**](../user-guide/architecture/system-overview.md) — understand the full architecture
- [**Data Model**](../user-guide/data-model/overview.md) — learn about event ordering and storage
- [**First Steps Tutorial**](../tutorials/native-linux/first-steps.md) — a hands-on walkthrough of recording and replaying events
