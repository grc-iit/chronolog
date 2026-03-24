---
sidebar_position: 4
title: "Core Types"
---

# Core Types

All types described on this page live in the `chronolog` namespace and are defined in headers under `chrono_common/include/` and `ChronoVisor/include/`.

## Primitive Type Aliases

These aliases provide semantic meaning to otherwise opaque integer and string types throughout the codebase.

| Alias | Underlying Type | Purpose |
|---|---|---|
| `StoryName` | `std::string` | Human-readable story name |
| `ChronicleName` | `std::string` | Human-readable chronicle name |
| `StoryId` | `uint64_t` | Numeric story identifier (CityHash64) |
| `ChronicleId` | `uint64_t` | Numeric chronicle identifier (CityHash64) |
| `ClientId` | `uint64_t` | Client process identifier |
| `chrono_time` | `uint64_t` | Timestamp value |
| `chrono_index` | `uint32_t` | Per-client event index |

**Source:** `chrono_common/include/chronolog_types.h`

## Ordering Tuples

ChronoLog uses two tuple types to order events:

- **`EventSequence`** — `std::tuple<chrono_time, ClientId, chrono_index>` — the global ordering key used within `StoryChunk` to sort events across all clients. The three-part key is necessary for distributed uniqueness: multiple clients on different nodes may produce events at the same timestamp, and a single client may produce multiple events within the same timestamp granularity. The `ClientId` component distinguishes between clients, and `chrono_index` disambiguates within a single client.

- **`ArrivalSequence`** — `std::tuple<chrono_time, chrono_index>` — a local ordering key that omits the `ClientId`. Used in contexts where events are already scoped to a single client.

**Source:** `chrono_common/include/StoryChunk.h`

## LogEvent

The atomic data unit in ChronoLog. Every piece of data stored in the system is represented as a `LogEvent`.

| Field | Type | Description |
|---|---|---|
| `storyId` | `StoryId` (`uint64_t`) | Story this event belongs to |
| `eventTime` | `uint64_t` | Timestamp when the event was captured |
| `clientId` | `ClientId` (`uint64_t`) | Client process that generated the event |
| `eventIndex` | `uint32_t` | Per-client counter for timestamp disambiguation |
| `logRecord` | `std::string` | Opaque application payload |

**Equality:** Two `LogEvent` instances are equal when `(storyId, eventTime, clientId, eventIndex)` all match. The `logRecord` payload is intentionally excluded from comparison — two events at the same sequence point are considered the same event regardless of payload content.

**Serialization:** Implements `template<typename SerArchiveT> void serialize(SerArchiveT&)` for Thallium RPC transport, serializing all five fields.

**Source:** `chrono_common/include/chronolog_types.h`

## StoryChunk

A time-windowed container for `LogEvent` instances belonging to a single Story. Events are stored in a `std::map<EventSequence, LogEvent>`, which keeps them sorted by global sequence order at all times.

| Field | Type | Description |
|---|---|---|
| `chronicleName` | `ChronicleName` | Parent chronicle name |
| `storyName` | `StoryName` | Story name |
| `storyId` | `StoryId` | Numeric story identifier |
| `startTime` | `uint64_t` | Start of the time window (inclusive) |
| `endTime` | `uint64_t` | End of the time window (exclusive) |
| `revisionTime` | `uint64_t` | Last modification timestamp |
| `logEvents` | `std::map<EventSequence, LogEvent>` | Sorted event container |

**Time-window invariant:** A StoryChunk only accepts events where `startTime <= eventTime < endTime`. This invariant is enforced at insertion time.

### Key Operations

- **`insertEvent(LogEvent const&)`** — Inserts a single event into the sorted map.
- **`mergeEvents(std::map<...>& events, iterator& merge_start)`** — Merges events from another map starting at the given iterator position. Used when combining partial StoryChunks from different ChronoKeepers.
- **`mergeEvents(StoryChunk& other_chunk, uint64_t start_time)`** — Merges all events from another StoryChunk, optionally starting from a given time. This is the primary merge path used by ChronoGrapher.
- **`eraseEvents(iterator first, iterator last)`** / **`eraseEvents(uint64_t start, uint64_t end)`** — Removes events by iterator range or time range.
- **`extractEventSeries(std::vector<Event>&)`** — Extracts events into a flat vector for client consumption.

**Serialization:** Implements Thallium `serialize()` covering all fields including the event map.

**Source:** `chrono_common/include/StoryChunk.h`

## Story (ChronoVisor In-Memory Representation)

The `Story` class in ChronoVisor represents the in-memory state of a Story, including its configuration and the set of clients that have acquired it.

### Configuration Enums

**`StoryIndexingGranularity`** — Controls the time resolution for event indexing:

| Value | Name | Meaning |
|---|---|---|
| 0 | `story_gran_ns` | Nanosecond granularity |
| 1 | `story_gran_us` | Microsecond granularity |
| 2 | `story_gran_ms` | Millisecond granularity (default) |
| 3 | `story_gran_sec` | Second granularity |

**`StoryType`** — Categorizes the Story for scheduling purposes:

| Value | Name | Meaning |
|---|---|---|
| 0 | `story_type_standard` | Standard story (default) |
| 1 | `story_type_priority` | Priority story |

**`StoryTieringPolicy`** — Controls storage placement:

| Value | Name | Meaning |
|---|---|---|
| 0 | `story_tiering_normal` | Normal tiering (default) |
| 1 | `story_tiering_hot` | Hot tiering (keep in fast storage) |
| 2 | `story_tiering_cold` | Cold tiering (move to archival storage) |

### StoryAttrs

The `StoryAttrs` struct bundles the configurable attributes:

| Field | Type | Description |
|---|---|---|
| `size` | `uint64_t` | Story size |
| `indexing_granularity` | `StoryIndexingGranularity` | Time resolution |
| `type` | `StoryType` | Standard or priority |
| `tiering_policy` | `StoryTieringPolicy` | Storage placement policy |
| `access_permission` | `uint16_t` | Access control flags |

### StoryStats

The `StoryStats` struct contains a single field:

| Field | Type | Description |
|---|---|---|
| `count` | `uint64_t` | Acquisition reference count |

### Client Tracking

The Story maintains an `acquirerClientMap_` (`std::unordered_map<ClientId, ClientInfo*>`) protected by a `std::mutex`. This map tracks which clients have acquired the Story and provides thread-safe `addAcquirerClient()` and `removeAcquirerClient()` operations. The acquisition count (`stats_.count`) acts as a reference counter — a Story cannot be destroyed while its acquisition count is non-zero.

**Source:** `ChronoVisor/include/Story.h`

## Chronicle (ChronoVisor In-Memory Representation)

The `Chronicle` class mirrors the Story pattern with its own set of configuration enums and attributes.

### Configuration Enums

Chronicles use the same three-enum pattern as Stories:

- **`ChronicleIndexingGranularity`** — `chronicle_gran_ns` (0), `chronicle_gran_us` (1), `chronicle_gran_ms` (2), `chronicle_gran_sec` (3)
- **`ChronicleType`** — `chronicle_type_standard` (0), `chronicle_type_priority` (1)
- **`ChronicleTieringPolicy`** — `chronicle_tiering_normal` (0), `chronicle_tiering_hot` (1), `chronicle_tiering_cold` (2)

### ChronicleAttrs

| Field | Type | Description |
|---|---|---|
| `size` | `uint64_t` | Chronicle size |
| `indexing_granularity` | `ChronicleIndexingGranularity` | Time resolution |
| `type` | `ChronicleType` | Standard or priority |
| `tiering_policy` | `ChronicleTieringPolicy` | Storage placement policy |
| `access_permission` | `uint16_t` | Access control flags |

### Capacity Limits

Chronicles enforce the following maximum sizes:

| Container | Limit | Macro |
|---|---|---|
| Property list | 16 entries | `MAX_CHRONICLE_PROPERTY_LIST_SIZE` |
| Metadata map | 16 entries | `MAX_CHRONICLE_METADATA_MAP_SIZE` |
| Story map | 1024 entries | `MAX_STORY_MAP_SIZE` |
| Archive map | 1024 entries | `MAX_ARCHIVE_MAP_SIZE` |

### ID Generation

Story IDs are scoped within their parent Chronicle: `StoryId = CityHash64(chronicleName + storyName)`. This means the same story name in different Chronicles produces different IDs. The Chronicle itself is identified by `ChronicleId = CityHash64(chronicleName)`.

**Source:** `ChronoVisor/include/Chronicle.h`

## Archive

An Archive provides historical storage associated with a Chronicle. The `Archive` class is straightforward:

| Field | Type | Description |
|---|---|---|
| `name_` | `std::string` | Archive name |
| `aid_` | `uint64_t` | Archive ID |
| `cid_` | `uint64_t` | Parent Chronicle ID |
| `propertyList_` | `std::unordered_map<std::string, std::string>` | User-defined properties |

**ID generation:** `ArchiveId = CityHash64(toString(chronicleId) + archiveName)`, following the same scoping pattern as Stories.

**Source:** `ChronoVisor/include/Archive.h`

## Source File Reference

| Type | Header |
|---|---|
| Primitive aliases, `LogEvent` | `chrono_common/include/chronolog_types.h` |
| `EventSequence`, `ArrivalSequence`, `StoryChunk` | `chrono_common/include/StoryChunk.h` |
| `Story`, `StoryAttrs`, `StoryStats` | `ChronoVisor/include/Story.h` |
| `Chronicle`, `ChronicleAttrs`, `ChronicleStats` | `ChronoVisor/include/Chronicle.h` |
| `Archive` | `ChronoVisor/include/Archive.h` |
