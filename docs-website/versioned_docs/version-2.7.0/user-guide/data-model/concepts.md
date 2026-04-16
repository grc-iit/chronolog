---
sidebar_position: 2
title: "Concepts"
---

# Core Concepts

This page provides detailed definitions of ChronoLog's core data model concepts. For a high-level overview, see the [Overview](./overview.md).

## Conceptual Hierarchy

<svg viewBox="0 0 720 320" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="320" rx="10" fill="#1e2330"/>

  {/* Title */}
  <text x="360" y="20" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">CONCEPTUAL HIERARCHY</text>

  {/* Chronicle — client-visible container */}
  <rect x="40" y="30" width="640" height="275" rx="6" fill="none" stroke="#c3e04d" strokeWidth="1.2" strokeOpacity="0.8"/>
  <circle cx="56" cy="46" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="64" y="50" fill="#c3e04d" fontSize="10" fontWeight="600">Chronicle</text>
  <text x="145" y="50" fill="#9ca3b0" fontSize="7" fontStyle="italic">client-level namespace / container</text>

  {/* Story — client-visible dataset */}
  <rect x="70" y="60" width="580" height="225" rx="6" fill="none" stroke="#c3e04d" strokeWidth="1.1" strokeOpacity="0.75"/>
  <circle cx="86" cy="76" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="94" y="80" fill="#c3e04d" fontSize="10" fontWeight="600">Story</text>
  <text x="140" y="80" fill="#9ca3b0" fontSize="7" fontStyle="italic">client-level time-series dataset</text>

  {/* StoryChunk — service-internal */}
  <rect x="100" y="90" width="520" height="140" rx="6" fill="none" stroke="#c3e04d" strokeWidth="0.85" strokeDasharray="6,3" strokeOpacity="0.45"/>
  <circle cx="116" cy="106" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="124" y="110" fill="#8f9aab" fontSize="10" fontWeight="600">StoryChunk</text>
  <text x="200" y="110" fill="#7e8796" fontSize="7" fontStyle="italic">service-internal time-windowed event batch</text>

  {/* Event — client-visible atomic unit */}
  <rect x="130" y="128" width="460" height="90" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.55"/>
  <circle cx="146" cy="142" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="154" y="146" fill="#c3e04d" fontSize="10" fontWeight="600">Event</text>
  <text x="192" y="146" fill="#9ca3b0" fontSize="7" fontStyle="italic">client-visible individual record</text>

  {/* Event fields */}
  <rect x="150" y="156" width="100" height="22" rx="4" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="200" y="170" textAnchor="middle" fill="#9ca3b0" fontSize="7">storyId</text>

  <rect x="260" y="156" width="100" height="22" rx="4" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="310" y="170" textAnchor="middle" fill="#9ca3b0" fontSize="7">eventTime</text>

  <rect x="370" y="156" width="100" height="22" rx="4" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="420" y="170" textAnchor="middle" fill="#9ca3b0" fontSize="7">clientId</text>

  <rect x="480" y="156" width="100" height="22" rx="4" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="530" y="170" textAnchor="middle" fill="#9ca3b0" fontSize="7">eventIndex</text>

  <rect x="150" y="184" width="430" height="22" rx="4" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="365" y="198" textAnchor="middle" fill="#9ca3b0" fontSize="7">logRecord (application payload)</text>

  {/* Archive boxes at bottom of Story */}
  <rect x="100" y="244" width="110" height="30" rx="4" fill="none" stroke="#c3e04d" strokeWidth="0.8" strokeDasharray="5,3" strokeOpacity="0.45"/>
  <text x="155" y="262" textAnchor="middle" fill="#7d8a9c" fontSize="8">Archive</text>
  <rect x="237" y="244" width="110" height="30" rx="4" fill="none" stroke="#c3e04d" strokeWidth="0.8" strokeDasharray="5,3" strokeOpacity="0.45"/>
  <text x="292" y="262" textAnchor="middle" fill="#7d8a9c" fontSize="8">Archive</text>
  <rect x="373" y="244" width="110" height="30" rx="4" fill="none" stroke="#c3e04d" strokeWidth="0.8" strokeDasharray="5,3" strokeOpacity="0.45"/>
  <text x="428" y="262" textAnchor="middle" fill="#7d8a9c" fontSize="8">Archive</text>
  <rect x="510" y="244" width="110" height="30" rx="4" fill="none" stroke="#c3e04d" strokeWidth="0.8" strokeDasharray="5,3" strokeOpacity="0.45"/>
  <text x="565" y="262" textAnchor="middle" fill="#7d8a9c" fontSize="8">Archive</text>

  {/* Ordering annotation */}
  <text x="365" y="214" textAnchor="middle" fill="#c3e04d" fontSize="6" fillOpacity="0.6">ordered by EventSequence (eventTime, clientId, eventIndex)</text>
</svg>

Client-facing developers primarily interact with **Chronicle**, **Story**, and **Event**. System and infrastructure developers also work with internal concepts such as **StoryChunk** and **Archive**.

## Client-Level Concepts

### Chronicle

A Chronicle is the top-level organizational unit in ChronoLog — a named container that groups related Stories together for easier data management. Each Chronicle is uniquely identified by a numeric `ChronicleId`, computed as `CityHash64(chronicleName)`. This deterministic hashing means that any process in the distributed system can independently derive the same ID from the same name without coordination.

Chronicles expose configurable attributes including indexing granularity (nanoseconds through seconds), type (standard or priority), and tiering policy (normal, hot, or cold). In ChronoLog v2.7.0, these Chronicle-level attributes are accepted but do not affect runtime behavior yet. Chronicles also support user-defined properties and metadata, each capped at 16 entries.

Story names are scoped within their parent Chronicle, so two different Chronicles may each contain a Story with the same name without conflict. The Chronicle maintains an acquisition reference count that tracks how many clients are currently using it.

### Story

A Story is a time-series dataset within a Chronicle. It represents a logical stream of events that client applications generate over time. Each Story is uniquely identified by a `StoryId`, computed as `CityHash64(chronicleName + storyName)`. Concatenating the Chronicle name ensures that identically named Stories in different Chronicles receive distinct IDs.

Stories expose configurable attributes for indexing granularity, type, tiering policy, and access permission. In ChronoLog v2.7.0, these Story-level attributes are accepted but do not affect runtime behavior yet.

The ChronoVisor maintains an in-memory representation of each Story that tracks which clients have acquired (are actively writing to) the Story. This acquirer client map is protected by a mutex for thread-safe concurrent access. The Story's acquisition count acts as a reference counter — a Story cannot be removed while any client holds it.

### Event

An Event is the atomic data unit in ChronoLog. Every piece of data stored in the system is represented as an Event. Each event carries five fields: the `storyId` identifying which Story it belongs to, an `eventTime` timestamp, the `clientId` of the application that generated it, an `eventIndex` (per-client counter to disambiguate events with identical timestamps), and a `logRecord` containing the opaque application payload.

Events are globally ordered by a composite key called `EventSequence`, which is the tuple `(eventTime, clientId, eventIndex)`. This three-part key is essential for distributed uniqueness: because multiple clients on different HPC nodes may generate events at the same timestamp, the `clientId` component distinguishes them. The `eventIndex` further handles the case where the same client produces multiple events within a single timestamp granularity (e.g., from different threads). Equality comparison checks all identifying fields (`storyId`, `eventTime`, `clientId`, `eventIndex`) but deliberately excludes the `logRecord` payload.

## Service-Internal Concepts

### StoryChunk

A StoryChunk is a container for Events that fall within a specific time window `[startTime, endTime)` — including the start time and excluding the end time. Events within a StoryChunk are stored in a sorted map keyed by `EventSequence`, ensuring they are always maintained in global order.

StoryChunks are the unit of data movement through the distributed pipeline. ChronoKeeper processes on compute nodes collect events into StoryChunks during a configurable acceptance window. When the window expires, the StoryChunk is forwarded to a ChronoGrapher process on a storage node. Because multiple ChronoKeepers may handle events for the same Story, each produces a partial StoryChunk for overlapping time ranges. The ChronoGrapher merges these partial chunks into its own StoryPipeline — a sequence of StoryChunks ordered by start time.

Each StoryChunk also carries a `revisionTime` field that records when it was last modified, enabling the system to track data freshness during the merge process.

### Archive

An Archive provides historical storage associated with a Chronicle. Each Archive is identified by an `ArchiveId`, computed as `CityHash64(toString(chronicleId) + archiveName)`. Archives carry a user-defined property list for application-specific metadata. The list has no effect so far in ChronoLog v2.7.0.
