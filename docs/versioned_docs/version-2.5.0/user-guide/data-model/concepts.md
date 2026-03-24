---
sidebar_position: 2
title: "Concepts"
---

# Core Concepts

This page provides detailed definitions of ChronoLog's core data model concepts. For a high-level overview, see the [Overview](./overview.md).

## Conceptual Hierarchy

<svg viewBox="0 0 720 280" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="280" rx="10" fill="#1e2330"/>

  {/* Title */}
  <text x="360" y="20" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">CONCEPTUAL HIERARCHY</text>

  {/* Chronicle — outermost container */}
  <rect x="40" y="30" width="640" height="235" rx="6" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="8,4" strokeOpacity="0.4"/>
  <circle cx="56" cy="46" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="64" y="50" fill="#c3e04d" fontSize="10" fontWeight="600">Chronicle</text>
  <text x="145" y="50" fill="#9ca3b0" fontSize="7" fontStyle="italic">namespace / container</text>

  {/* Story — second level */}
  <rect x="70" y="60" width="580" height="195" rx="6" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.35"/>
  <circle cx="86" cy="76" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="94" y="80" fill="#c3e04d" fontSize="10" fontWeight="600">Story</text>
  <text x="140" y="80" fill="#9ca3b0" fontSize="7" fontStyle="italic">time-series dataset</text>

  {/* StoryChunk — third level */}
  <rect x="100" y="90" width="520" height="155" rx="6" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.3"/>
  <circle cx="116" cy="106" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="124" y="110" fill="#c3e04d" fontSize="10" fontWeight="600">StoryChunk</text>
  <text x="200" y="110" fill="#9ca3b0" fontSize="7" fontStyle="italic">time-windowed event batch</text>

  {/* LogEvent — innermost element */}
  <rect x="130" y="120" width="460" height="115" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="146" cy="136" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="154" y="140" fill="#c3e04d" fontSize="10" fontWeight="600">LogEvent</text>
  <text x="215" y="140" fill="#9ca3b0" fontSize="7" fontStyle="italic">individual record</text>

  {/* LogEvent fields */}
  <rect x="150" y="152" width="100" height="22" rx="4" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="200" y="166" textAnchor="middle" fill="#9ca3b0" fontSize="7">storyId</text>

  <rect x="260" y="152" width="100" height="22" rx="4" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="310" y="166" textAnchor="middle" fill="#9ca3b0" fontSize="7">eventTime</text>

  <rect x="370" y="152" width="100" height="22" rx="4" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="420" y="166" textAnchor="middle" fill="#9ca3b0" fontSize="7">clientId</text>

  <rect x="480" y="152" width="100" height="22" rx="4" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="530" y="166" textAnchor="middle" fill="#9ca3b0" fontSize="7">eventIndex</text>

  <rect x="150" y="180" width="430" height="22" rx="4" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="365" y="194" textAnchor="middle" fill="#9ca3b0" fontSize="7">logRecord (application payload)</text>

  {/* Ordering annotation */}
  <text x="365" y="218" textAnchor="middle" fill="#c3e04d" fontSize="6" fillOpacity="0.6">ordered by EventSequence (eventTime, clientId, eventIndex)</text>
</svg>

An additional concept, **Archive**, provides historical storage associated with a Chronicle.

## Chronicle

A Chronicle is the top-level organizational unit in ChronoLog — a named container that groups related Stories together for easier data management. Each Chronicle is uniquely identified by a numeric `ChronicleId`, computed as `CityHash64(chronicleName)`. This deterministic hashing means that any process in the distributed system can independently derive the same ID from the same name without coordination.

Chronicles carry configurable attributes including indexing granularity (nanoseconds through seconds), type (standard or priority), and tiering policy (normal, hot, or cold). They also support user-defined properties and metadata, each capped at 16 entries, enabling application-specific annotations.

A single Chronicle can hold up to 1024 Stories and 1024 Archives. Story names are scoped within their parent Chronicle, so two different Chronicles may each contain a Story with the same name without conflict. The Chronicle maintains an acquisition reference count that tracks how many clients are currently using it.

## Story

A Story is a time-series dataset within a Chronicle. It represents a logical stream of events that client applications generate over time. Each Story is uniquely identified by a `StoryId`, computed as `CityHash64(chronicleName + storyName)`. Concatenating the Chronicle name ensures that identically named Stories in different Chronicles receive distinct IDs.

Stories have configurable attributes that control their behavior in the pipeline. The indexing granularity (nanosecond, microsecond, millisecond, or second) determines time resolution. The type (standard or priority) affects scheduling, and the tiering policy (normal, hot, or cold) influences storage placement decisions. An access permission field provides basic authorization control.

The ChronoVisor maintains an in-memory representation of each Story that tracks which clients have acquired (are actively writing to) the Story. This acquirer client map is protected by a mutex for thread-safe concurrent access. The Story's acquisition count acts as a reference counter — a Story cannot be removed while any client holds it.

## LogEvent

A LogEvent is the atomic data unit in ChronoLog. Every piece of data stored in the system is represented as a LogEvent. Each event carries five fields: the `storyId` identifying which Story it belongs to, an `eventTime` timestamp, the `clientId` of the application that generated it, an `eventIndex` (per-client counter to disambiguate events with identical timestamps), and a `logRecord` containing the opaque application payload.

Events are globally ordered by a composite key called `EventSequence`, which is the tuple `(eventTime, clientId, eventIndex)`. This three-part key is essential for distributed uniqueness: because multiple clients on different HPC nodes may generate events at the same timestamp, the `clientId` component distinguishes them. The `eventIndex` further handles the case where the same client produces multiple events within a single timestamp granularity (e.g., from different threads). Equality comparison checks all identifying fields (`storyId`, `eventTime`, `clientId`, `eventIndex`) but deliberately excludes the `logRecord` payload.

## StoryChunk

A StoryChunk is a container for LogEvents that fall within a specific time window `[startTime, endTime)` — including the start time and excluding the end time. Events within a StoryChunk are stored in a sorted map keyed by `EventSequence`, ensuring they are always maintained in global order.

StoryChunks are the unit of data movement through the distributed pipeline. ChronoKeeper processes on compute nodes collect events into StoryChunks during a configurable acceptance window. When the window expires, the StoryChunk is forwarded to a ChronoGrapher process on a storage node. Because multiple ChronoKeepers may handle events for the same Story, each produces a partial StoryChunk for overlapping time ranges. The ChronoGrapher merges these partial chunks into its own StoryPipeline — a sequence of StoryChunks ordered by start time.

Each StoryChunk also carries a `revisionTime` field that records when it was last modified, enabling the system to track data freshness during the merge process.

## Archive

An Archive provides historical storage associated with a Chronicle. Each Archive is identified by an `ArchiveId`, computed as `CityHash64(toString(chronicleId) + archiveName)`. Like Stories, Archive names are scoped within their parent Chronicle so that the same Archive name can be reused across different Chronicles. Archives carry a user-defined property list for application-specific metadata.
