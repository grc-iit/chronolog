---
sidebar_position: 2
title: "API Reference"
---

# Python Client API

All classes described here are part of the `py_chronolog_client` module.

```python
import py_chronolog_client
```

---

## `ClientPortalServiceConf` Class

Holds the connection parameters for the ChronoVisor portal service (used for chronicle and story management and event logging).

### Constructor

```python
ClientPortalServiceConf(proto, ip, port, provider_id)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `proto` | `str` | Transport protocol (e.g. `"ofi+sockets"`) |
| `ip` | `str` | IP address of the ChronoVisor portal service |
| `port` | `int` | Port number of the portal service |
| `provider_id` | `int` | Margo/Thallium provider ID |

**Default values** (matching `ClientConfiguration` defaults):

| Field | Default |
|-------|---------|
| `proto` | `"ofi+sockets"` |
| `ip` | `"127.0.0.1"` |
| `port` | `5555` |
| `provider_id` | `55` |

---

## `ClientQueryServiceConf` Class

Holds the connection parameters for the ChronoPlayer query service (used for story replay). Only needed when constructing a client in WRITER + READER mode.

### Constructor

```python
ClientQueryServiceConf(proto, ip, port, provider_id)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `proto` | `str` | Transport protocol (e.g. `"ofi+sockets"`) |
| `ip` | `str` | IP address of the ChronoPlayer query service |
| `port` | `int` | Port number of the query service |
| `provider_id` | `int` | Margo/Thallium provider ID |

**Default values**:

| Field | Default |
|-------|---------|
| `proto` | `"ofi+sockets"` |
| `ip` | `"127.0.0.1"` |
| `port` | `5557` |
| `provider_id` | `57` |

---

## `Client` Class

The `Client` class manages the connection to ChronoVisor and provides all chronicle, story, and event operations.

### Constructors

```python
# WRITER mode — event logging only
Client(portal_conf: ClientPortalServiceConf)

# WRITER + READER mode — event logging and story replay
Client(portal_conf: ClientPortalServiceConf, query_conf: ClientQueryServiceConf)
```

- The first constructor creates a write-only client connected to the ChronoVisor portal service.
- The second constructor additionally connects to the ChronoPlayer query service, enabling `ReplayStory`. Calling `ReplayStory` on a write-only client returns `-11` (`CL_ERR_NOT_READER_MODE`).

### Connection

```python
Client.Connect() -> int
```

- Establish a session with ChronoVisor. The client advertises its wire-protocol version as part of the handshake.
- Returns `0` (`CL_SUCCESS`) on success, `-13` (`CL_ERR_PROTOCOL_VERSION_MISMATCH`) if the server rejects the client's protocol version, or another error code on failure.

```python
Client.Disconnect() -> int
```

- Close the active session. Any stories still acquired by this client are auto-released by the Visor as part of disconnect.
- Returns `0` (`CL_SUCCESS`) on success, error code otherwise.

### Chronicle Management

```python
Client.CreateChronicle(chronicle_name: str) -> int
```

- Create a new chronicle named `chronicle_name`.
- Returns `0` (`CL_SUCCESS`) on success, `-6` (`CL_ERR_CHRONICLE_EXISTS`) if a chronicle with that name already exists, or another error code on failure.

```python
Client.DestroyChronicle(chronicle_name: str) -> int
```

- Permanently delete the chronicle, including all of its stories and any persisted archive data.
- Returns `0` (`CL_SUCCESS`) on success, `-4` (`CL_ERR_ACQUIRED`) if any story within the chronicle is currently acquired (by any client), or another error code on failure.

```python
Client.ShowChronicles() -> tuple[int, list[str]]
```

- Returns a tuple `(return_code, names)`. On success, `return_code` is `0` and `names` is the list of all chronicles visible to this client. On failure, `names` is empty.

### Story Management

```python
Client.AcquireStory(chronicle_name: str,
                    story_name: str) -> tuple[int, StoryHandle]
```

- Open the story `story_name` inside `chronicle_name` for data access. If the story does not exist it is created.
- Returns a tuple `(return_code, handle)`. On success, `return_code` is `0` and `handle` is a valid `StoryHandle`; on failure, `handle` is `None`.
- Returns `-3` (`CL_ERR_NOT_EXIST`) if the chronicle does not exist.

```python
Client.ReleaseStory(chronicle_name: str,
                    story_name: str) -> int
```

- Release the story handle obtained from `AcquireStory`. Pending events buffered by this client are flushed before the call returns.
- Returns `0` (`CL_SUCCESS`) on success, `-5` (`CL_ERR_NOT_ACQUIRED`) if the story was not acquired by this client.

```python
Client.DestroyStory(chronicle_name: str,
                    story_name: str) -> int
```

- Permanently delete the story, including any persisted archive data.
- Returns `0` (`CL_SUCCESS`) on success, `-4` (`CL_ERR_ACQUIRED`) if the story is currently acquired (by any client), or another error code on failure.

```python
Client.ShowStories(chronicle_name: str) -> tuple[int, list[str]]
```

- Returns a tuple `(return_code, names)`. On success, `return_code` is `0` and `names` is the list of all stories in `chronicle_name`. On failure (e.g. `-3` `CL_ERR_NOT_EXIST` if the chronicle does not exist), `names` is empty.

### Event Replay

```python
Client.ReplayStory(chronicle_name: str,
                   story_name: str,
                   start: int,
                   end: int,
                   event_series: EventList) -> int
```

- Retrieve all events in the story whose timestamps fall within the half-open range `[start, end)`.
- Events are appended to `event_series` (an `EventList` instance that must be created by the caller) in chronological order.
- Returns `0` (`CL_SUCCESS`) on success, `-11` (`CL_ERR_NOT_READER_MODE`) if the client was constructed without a `ClientQueryServiceConf`, `-12` (`CL_ERR_QUERY_TIMED_OUT`) if the query did not complete in time, or another error code on failure.
- Returns `-18` (`CL_ERR_PARTIAL_RESULT`) when ChronoPlayer answered but knows the series is short: a keeper of the story did not answer within its 5-second fetch deadline, a keeper's answer hit its per-query event cap, or an archive file in the range could not be read. `event_series` holds the events it did gather; retry the range to pick up the rest.

---

## `StoryHandle` Class

`StoryHandle` is obtained from `Client.AcquireStory` and provides write access as well as low-latency on-demand tail reading for a story.

```python
StoryHandle.log_event(event_string: str) -> int
```

- Append a new event with the log record `event_string`.
- Returns the `uint64_t` timestamp (as a Python `int`) assigned to the event by the client library.
- The keeper's answer is discarded; a keeper crash before ChronoGrapher writes the event loses it.
  Use the overload below with `Durability.Durable` when the call must not return until the event is
  on the keeper's disk.

```python
StoryHandle.log_event(log_string: str, options: LogEventOptions) -> tuple[int, int]
```

- Append a new event and wait until it has reached `options.durability`. Returns a tuple
  `(status, event_time)`. `status` is `0` (`CL_SUCCESS`) once the event got there; `event_time` is
  the event's timestamp on success and with `-21`, and `0` with every other status.
- With `Durability.Accepted` (the default) it takes the same path as `log_event(event_string)` and
  returns the keeper's status instead of discarding it.
- With `Durability.Durable` it returns `0` only after the keeper wrote the event to its write-ahead
  log (WAL) and `fdatasync` returned. A keeper that cannot make the event durable answers with an
  error and stores nothing, except that `-21` means it may be stored; the write is never downgraded
  to Accepted. The WAL is off by default on the keepers, and with it off every durable write
  returns `-19` (`CL_ERR_DURABILITY_UNAVAILABLE`). See
  [Keeper WAL](../../user-guide/architecture/keeper-wal.md).
- A durable call can block for up to `options.attempts × options.timeout_ms` (30 s with the
  defaults). It releases the GIL while it waits, so other Python threads keep running. Do not change
  the `options` object from another thread during the call.
- Retries, at-least-once storage and the return codes are the same as in C++: see
  [the C++ API](../cpp/api.md#storyhandle-class) and [Error Codes](./error-codes.md#durability-error-codes).
  In short, every retry carries the same `(time, client id, index)`, copies collapse on read, so a
  reader sees the event once; `-2` means a bad argument (an empty record, `attempts` or
  `timeout_ms` of 0), `-7` no keepers, and `-19`, `-20`, `-21` are the durability codes.

```python
options = py_chronolog_client.LogEventOptions()
options.durability = py_chronolog_client.Durability.Durable

status, event_time = story.log_event("sensor reading 42", options)
if status == 0:
    pass                      # on the keeper's disk
elif status == -20:
    pass                      # nothing stored; back off and write again
elif status == -21:
    pass                      # may be stored; a new log_event creates a second event.
                              # event_time is the event's timestamp: replay the story
                              # around it to see whether the event is there
else:
    print(f"durable write failed: {status}")   # nothing stored
```

A Python subclass of `StoryHandle` that overrides `log_event` serves both forms with one method, so
the override must accept an optional `options` argument and, when given one, return the
`(status, event_time)` pair. An override that takes only `event_string` raises `TypeError` when
called as `log_event(record, options)`.

```python
StoryHandle.playback(n: int, events: EventList) -> int
```

- **On-Demand Tail Read**: Retrieve the most recent `n` events of this story directly from ChronoKeeper in-memory buffers (`KeeperChunkRetentionStore`), bypassing the ChronoPlayer/HDF5 archive path.
- `events` is an `EventList` instance populated in place with the returned `Event` objects in ascending order.
- Returns `0` (`CL_SUCCESS`) on success, `-7` (`CL_ERR_NO_KEEPERS`), `-12` (`CL_ERR_QUERY_TIMED_OUT`), `-18` (`CL_ERR_PARTIAL_RESULT`), or `-1` (`CL_ERR_UNKNOWN`).
- Supported in writer-only client mode (`ClientPortalServiceConf` only).


---

## `Durability` Enum

How far an event must get before `StoryHandle.log_event(log_string, options)` returns `0`.

| Value | Meaning |
|-------|---------|
| `Durability.Accepted` | In keeper memory; what `log_event(event_string)` gives. The default. |
| `Durability.Durable` | Written to the keeper's WAL and `fdatasync`'d. |

---

## `LogEventOptions` Class

```python
options = py_chronolog_client.LogEventOptions()
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `durability` | `Durability` | `Durability.Accepted` | Level the call waits for |
| `timeout_ms` | `int` | `10000` | Durable writes only: deadline of each attempt, in milliseconds |
| `attempts` | `int` | `3` | Durable writes only: attempts to send before giving up; an attempt that gets no answer is retried |

---

## `Event` Class

`Event` is the smallest data unit in ChronoLog. Events are returned by `Client.ReplayStory` inside an `EventList`.

### Constructor

```python
Event(event_time: int = 0,
      client_id: int = 0,
      index: int = 0,
      log_record: str = "")
```

### Getters

```python
Event.time()       -> int   # event timestamp (uint64_t)
Event.client_id()  -> int   # originating client ID
Event.index()      -> int   # per-client sequence index (uint32_t)
Event.log_record() -> str   # the log record string
```

---

## `EventList` Class

`EventList` is the container returned (by reference) by `Client.ReplayStory`. Create an empty instance before calling `ReplayStory` and pass it as the last argument.

```python
event_series = py_chronolog_client.EventList()
ret = client.ReplayStory("MyChronicle", "MyStory", start, end, event_series)

print(len(event_series))   # number of events replayed

for event in event_series:
    print(event.time(), event.client_id(), event.index(), event.log_record())
```

---

See [Error Codes](./error-codes.md) for the full list of integer return codes produced by the methods above.
