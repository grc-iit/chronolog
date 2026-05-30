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

---

## `StoryHandle` Class

`StoryHandle` is obtained from `Client.AcquireStory` and provides write access to a story.

```python
StoryHandle.log_event(event_string: str) -> int
```

- Append a new event with the log record `event_string`.
- Returns the `uint64_t` timestamp (as a Python `int`) assigned to the event by the client library.

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
