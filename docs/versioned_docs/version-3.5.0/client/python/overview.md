---
sidebar_position: 1
title: "Overview"
---

# Python Client Overview

The **Python Client** is a [pybind11](https://pybind11.readthedocs.io/) binding that exposes the ChronoLog C++ Client API as the `py_chronolog_client` Python module. It provides the same chronicle, story, and event operations as the C++ client, allowing Python applications to interact with ChronoLog without writing any C++ code.

### Key Features

- Direct pybind11 binding over the native C++ client — no extra serialization layer.
- Supports both writer-only and writer+reader client modes.
- On-demand in-memory tail reads (`StoryHandle.playback`) for millisecond-scale retrieval of the latest events directly from keeper RAM.
- Chronicle and story lifecycle management, event logging, and historical story replay.
- Ideal for prototyping, data analysis, and integration into Python-based pipelines.

### Installation and Setup

1. Build and install ChronoLog. The install puts `py_chronolog_client` in the `lib` directory of the installation.
2. Add that directory to `PYTHONPATH`:
   ```bash
   export PYTHONPATH=/path/to/chronolog/lib:$PYTHONPATH
   ```
3. Run Python from ChronoLog's Spack environment, `/path/to/ChronoLog/.spack-env/view/bin/python3`. The module is built for that Python (3.14 on the 3.5.0 stack), and no other Python can import it. Its library paths are set at install, so it needs no `LD_LIBRARY_PATH`, and its file name needs no symbolic link.

### Client Modes

The `Client` class supports two modes depending on which constructor is used:

| Mode | Constructor | Capabilities |
|------|-------------|-------------|
| **WRITER_MODE** | `Client(ClientPortalServiceConf)` | Event logging only |
| **WRITER + READER MODE** | `Client(ClientPortalServiceConf, ClientQueryServiceConf)` | Event logging and story replay |

Use the single-argument constructor when you only need to produce events. Pass both a portal and a query service configuration to also enable `ReplayStory`.

### Connection Lifecycle

A typical session follows this sequence:

1. **Connect** — establish a session with ChronoVisor.
2. **CreateChronicle** — create a named chronicle (top-level namespace).
3. **AcquireStory** — open a story within the chronicle and obtain a `StoryHandle`.
4. **log_event** — append events through the `StoryHandle`. With `LogEventOptions` set to `Durability.Durable` the call returns `CL_SUCCESS` only once the keeper has synced the event to its write-ahead log (see [`LogEventOptions`](./api.md#logeventoptions-class)).
5. **playback** — retrieve the most recent $N$ events from keeper memory (writer or reader mode).
6. **ReplayStory** — read back historical events within a time range (READER mode only).
7. **ReleaseStory** — release the story handle.
8. **DestroyStory / DestroyChronicle** — clean up resources when no longer needed.
9. **Disconnect** — close the session.

```python
import py_chronolog_client

portal_conf = py_chronolog_client.ClientPortalServiceConf("ofi+sockets", "127.0.0.1", 5555, 55)
client = py_chronolog_client.Client(portal_conf)
client.Connect()

client.CreateChronicle("MyChronicle")

ret, handle = client.AcquireStory("MyChronicle", "MyStory")
handle.log_event("hello world")

client.ReleaseStory("MyChronicle", "MyStory")
client.Disconnect()
```

### Next Steps

- [API Reference](./api.md) — full method signatures and return codes.
- [Error Codes](./error-codes.md) — complete list of integer return codes.
- [Examples](./examples.md) — annotated end-to-end examples including story replay.
