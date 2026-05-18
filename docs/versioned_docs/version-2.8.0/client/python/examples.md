---
sidebar_position: 4
title: "Examples"
---

# Python Client Examples

## Writer Client

This example demonstrates the basic writer lifecycle: connecting to ChronoVisor, creating a chronicle, acquiring a story, logging events, and cleaning up.

```python
import py_chronolog_client

print("Basic test for py_chronolog_client")

# Create ClientPortalServiceConf with connection credentials
client_conf = py_chronolog_client.ClientPortalServiceConf("ofi+sockets", "127.0.0.1", 5555, 55)

# Instantiate a write-only Client
client = py_chronolog_client.Client(client_conf)

# Connect to ChronoVisor
return_code = client.Connect()
print("\nclient.Connect() returned:", return_code)

attrs = {}

# Create a chronicle
return_code = client.CreateChronicle("py_chronicle", attrs, 1)
print("\nclient.CreateChronicle() returned:", return_code)

# Acquire a story within the chronicle
return_tuple = client.AcquireStory("py_chronicle", "my_story", attrs, 1)
print("\nclient.AcquireStory() returned:", return_tuple)

if return_tuple[0] == 0:
    print("\nAcquired story 'my_story' in chronicle 'py_chronicle'")

    # Log events through the StoryHandle
    story_handle = return_tuple[1]
    story_handle.log_event("py_event_1")
    story_handle.log_event("py_event_2")
    story_handle.log_event("py_event_3")
    story_handle.log_event("py_event_4")
    print("\nLogged 4 events to 'my_story'")

# Release the story (flushes buffered events)
return_code = client.ReleaseStory("py_chronicle", "my_story")
print("\nclient.ReleaseStory() returned:", return_code)

# Disconnect from ChronoVisor
return_code = client.Disconnect()
print("\nclient.Disconnect() returned:", return_code)
```

### Key Steps Explained

| Step | What it does |
|------|-------------|
| **Connect** | Opens a session with ChronoVisor over the configured transport. |
| **CreateChronicle** | Registers a top-level namespace. Returns `-6` (`CL_ERR_CHRONICLE_EXISTS`) if one already exists with that name, which is safe to ignore when you just want to ensure it exists. |
| **AcquireStory** | Opens (and creates if necessary) a story. Returns a `tuple` — check `[0]` for the return code and use `[1]` as the `StoryHandle`. |
| **log_event** | Appends an event record. The call is non-blocking; events are buffered and flushed in the background. |
| **ReleaseStory** | Flushes buffered events and releases the story handle. |

---

## Reader Client

This example demonstrates WRITER + READER mode: writing events and then replaying them back using `ReplayStory`. It corresponds to the `reader_client.py` file in the source tree.

```python
import py_chronolog_client

print("Basic test for py_chronolog_client reader")

# Configure both the portal service (writes) and the query service (reads)
client_conf = py_chronolog_client.ClientPortalServiceConf("ofi+sockets", "127.0.0.1", 5555, 55)
query_conf  = py_chronolog_client.ClientQueryServiceConf("ofi+sockets", "127.0.0.1", 5557, 57)

# Passing both configs enables WRITER + READER mode
client = py_chronolog_client.Client(client_conf, query_conf)

return_code = client.Connect()
print("\nclient.Connect() returned:", return_code)

attrs = {}

# Acquire the story to read from (must already exist and have events)
return_tuple = client.AcquireStory("py_chronicle", "my_story", attrs, 1)
print("\nclient.AcquireStory() returned:", return_tuple)

# Create an EventList to hold the replayed events
event_series = py_chronolog_client.EventList()

# Replay events within a time range [start, end] (uint64_t nanosecond timestamps)
start_time = 1750968060000000000
end_time   = 1750968200000000000
return_code = client.ReplayStory("py_chronicle", "my_story", start_time, end_time, event_series)

print("\nclient.ReplayStory() returned:", return_code)
print("Replayed", len(event_series), "event(s)")

if len(event_series) > 0:
    for event in event_series:
        print("event:", event.time(), event.client_id(), event.index(), event.log_record())

# Release the story
return_code = client.ReleaseStory("py_chronicle", "my_story")
print("\nclient.ReleaseStory() returned:", return_code)

# Disconnect
return_code = client.Disconnect()
print("\nclient.Disconnect() returned:", return_code)
```

### Key Steps Explained

| Step | What it does |
|------|-------------|
| **`ClientQueryServiceConf`** | Required for reader mode. Points to the ChronoPlayer query service (default port `5557`). |
| **`Client(client_conf, query_conf)`** | Passing both configs enables WRITER + READER mode and unlocks `ReplayStory`. |
| **`EventList()`** | An empty container that `ReplayStory` populates. Supports `len()` and iteration. |
| **`ReplayStory`** | Fetches all persisted events whose timestamps fall in `[start, end]`. Returns `-11` if the client is not in READER mode. |
| **`event.time()`** | The `uint64_t` timestamp assigned when the event was logged. |
| **`event.log_record()`** | The string payload that was passed to `log_event`. |

:::note
Events logged by `log_event` are buffered and flushed to storage asynchronously. Allow sufficient time between writing and replaying for the background flush to complete. Future versions may provide an explicit sync mechanism.
:::

### Expected Console Output

```
client.Connect() returned: 0
client.AcquireStory() returned: (0, <py_chronolog_client.StoryHandle object>)
client.ReplayStory() returned: 0
Replayed 3 event(s):
event: 1750968100000000001 42 0 py_event_1
event: 1750968100000000002 42 1 py_event_2
event: 1750968100000000003 42 2 py_event_3
client.ReleaseStory() returned: 0
client.Disconnect() returned: 0
```

Actual timestamps and client IDs will differ on each run.
