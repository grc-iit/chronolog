---
sidebar_position: 6
title: "Service Identities"
---

# Service Identities

ChronoLog's distributed architecture requires every service process to be uniquely identifiable so that other processes can establish RPC connections to it. This page documents the identity types and the RPC messages that carry them.

## ServiceId

The base identity for any network-accessible service in ChronoLog. A `ServiceId` uniquely identifies a Thallium RPC endpoint.

| Field | Type | Description |
|---|---|---|
| `protocol` | `std::string` | Transport protocol (e.g., `"ofi+sockets"`, `"ofi+tcp"`) |
| `ip_addr` | `uint32_t` | IPv4 address in **host** byte order |
| `port` | `uint16_t` | Port number in host byte order |
| `provider_id` | `uint16_t` | Thallium provider ID (distinguishes multiple providers on the same endpoint) |

**IP conversion utilities:** The `ServiceId` provides `get_ip_as_dotted_string()` to convert the internal `uint32_t` representation to a human-readable dotted-quad string, and `get_service_as_string()` to produce a full URI like `ofi+sockets://192.168.1.1:5000`. The static helper `ip_addr_from_dotted_string_to_uint32()` handles the reverse conversion using `inet_pton` / `ntohl`.

**Validity check:** `is_valid()` returns `true` only when all four fields are set — a non-empty protocol, non-zero IP address, non-zero port, and non-zero provider ID.

**Serialization:** Implements Thallium `serialize()` for RPC transport.

**Source:** `chrono_common/include/ServiceId.h`

## RecordingGroupId

```cpp
typedef uint32_t RecordingGroupId;
```

A numeric identifier that groups recording services for load balancing. ChronoKeepers and ChronoGraphers within the same recording group collaborate on event collection for the same set of Stories.

**Source:** `chrono_common/include/ServiceId.h`

## Service Identity Cards

Each ChronoLog service type has an identity card class that bundles its `RecordingGroupId` with a `ServiceId`. All identity cards implement Thallium `serialize()` for RPC transport.

### KeeperIdCard

Identifies a ChronoKeeper process.

| Field | Type | Description |
|---|---|---|
| `groupId` | `RecordingGroupId` | Recording group this Keeper belongs to |
| `recordingServiceId` | `ServiceId` | Network endpoint for the recording service |

**Source:** `chrono_common/include/KeeperIdCard.h`

### GrapherIdCard

Identifies a ChronoGrapher process. Same structure as `KeeperIdCard`.

| Field | Type | Description |
|---|---|---|
| `groupId` | `RecordingGroupId` | Recording group this Grapher belongs to |
| `recordingServiceId` | `ServiceId` | Network endpoint for the recording service |

**Source:** `chrono_common/include/GrapherIdCard.h`

### PlayerIdCard

Identifies a ChronoPlayer process (playback/query service). While it carries a `RecordingGroupId` field, Players primarily serve playback rather than recording.

| Field | Type | Description |
|---|---|---|
| `groupId` | `RecordingGroupId` | Group identifier |
| `playbackServiceId` | `ServiceId` | Network endpoint for the playback service |

**Source:** `chrono_common/include/PlayerIdCard.h`

## RPC Messages

RPC messages wrap identity cards with additional context for specific protocol exchanges. All messages implement Thallium `serialize()`.

### Registration Messages

When a service process starts, it registers itself with the ChronoVisor by sending a registration message containing its identity card and an admin service endpoint.

| Message | Fields | Purpose |
|---|---|---|
| `KeeperRegistrationMsg` | `KeeperIdCard` + `ServiceId` (admin) | ChronoKeeper announces itself |
| `GrapherRegistrationMsg` | `GrapherIdCard` + `ServiceId` (admin) | ChronoGrapher announces itself |
| `PlayerRegistrationMsg` | `PlayerIdCard` + `ServiceId` (admin) | ChronoPlayer announces itself |

The admin `ServiceId` provides a separate endpoint for management operations (e.g., story assignment, shutdown commands) distinct from the data recording endpoint in the identity card.

**Sources:** `chrono_common/include/KeeperRegistrationMsg.h`, `GrapherRegistrationMsg.h`, `PlayerRegistrationMsg.h`

### Stats Messages

Services periodically report their status to the ChronoVisor for load balancing and monitoring.

| Message | Fields | Purpose |
|---|---|---|
| `KeeperStatsMsg` | `KeeperIdCard` + `active_story_count` (`uint32_t`) | Keeper reports workload |
| `GrapherStatsMsg` | `GrapherIdCard` + `active_story_count` (`uint32_t`) | Grapher reports workload |
| `PlayerStatsMsg` | `PlayerIdCard` + `active_story_count` (`uint32_t`) | Player reports workload |

The `active_story_count` field tells the ChronoVisor how many Stories the service is currently handling, enabling informed load-balancing decisions when assigning new Stories.

**Sources:** `chrono_common/include/KeeperStatsMsg.h`, `GrapherStatsMsg.h`, `PlayerStatsMsg.h`

### AcquireStoryResponseMsg

Returned to a client when it acquires a Story. This message connects the client to the distributed pipeline by providing the set of ChronoKeepers it should send events to and the ChronoPlayer it can query for playback.

| Field | Type | Description |
|---|---|---|
| `error_code` | `int` | Operation result (`CL_SUCCESS` or error) |
| `storyId` | `StoryId` | Numeric ID of the acquired Story |
| `keepers` | `std::vector<KeeperIdCard>` | ChronoKeepers assigned for recording |
| `player` | `ServiceId` | ChronoPlayer assigned for playback |

This is arguably the most important response message in the system — it tells the client exactly where to send its events and where to read them back. The vector of Keepers enables the client to distribute its events across multiple ingestion endpoints for parallelism and fault tolerance.

**Source:** `chrono_common/include/AcquireStoryResponseMsg.h`

### ConnectResponseMsg

Returned to a client upon initial connection to the ChronoVisor.

| Field | Type | Description |
|---|---|---|
| `error_code` | `int` | Connection result |
| `clientId` | `ClientId` | Assigned client identifier |

The ChronoVisor assigns each connecting client a unique `ClientId` that the client uses to tag all subsequent events. This ID becomes part of the `EventSequence` ordering key, ensuring globally unique event identification.

**Source:** `chrono_common/include/ConnectResponseMsg.h`
