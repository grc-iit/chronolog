---
sidebar_position: 1
title: "Overview"
---

# Python Client Overview

The **Python Client** is a [pybind11](https://pybind11.readthedocs.io/) binding that exposes the ChronoLog C++ Client API as the `py_chronolog_client` Python module. It provides the same chronicle, story, and event operations as the C++ client, allowing Python applications to interact with ChronoLog without writing any C++ code.

### Key Features

- Direct pybind11 binding over the native C++ client — no extra serialization layer.
- Supports both writer-only and writer+reader client modes.
- Chronicle and story lifecycle management, event logging, and story replay.
- Ideal for prototyping, data analysis, and integration into Python-based pipelines.

### Installation and Setup

1. Build and install ChronoLog, ensuring `py_chronolog_client` is compiled and available in your library directory.
2. Update environment variables:
   - Add the library path to `LD_LIBRARY_PATH`:
     ```bash
     export LD_LIBRARY_PATH=/path/to/chronolog/lib:$LD_LIBRARY_PATH
     ```
   - Add the library path to `PYTHONPATH`:
     ```bash
     export PYTHONPATH=/path/to/chronolog/lib:$PYTHONPATH
     ```
3. Create a symbolic link for the Python client:
   ```bash
   ln -s /path/to/chronolog/lib/py_chronolog_client.[python-version-linux-version].so /path/to/chronolog/lib/py_chronolog_client.so
   ```

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
4. **log_event** — append events through the `StoryHandle`.
5. **ReplayStory** — read back events within a time range (READER mode only).
6. **ReleaseStory** — release the story handle.
7. **DestroyStory / DestroyChronicle** — clean up resources when no longer needed.
8. **Disconnect** — close the session.

```python
import py_chronolog_client

portal_conf = py_chronolog_client.ClientPortalServiceConf("ofi+sockets", "127.0.0.1", 5555, 55)
client = py_chronolog_client.Client(portal_conf)
client.Connect()

attrs = {}
client.CreateChronicle("MyChronicle", attrs, 1)

ret, handle = client.AcquireStory("MyChronicle", "MyStory", attrs, 1)
handle.log_event("hello world")

client.ReleaseStory("MyChronicle", "MyStory")
client.Disconnect()
```

### Next Steps

- [API Reference](./api.md) — full method signatures and return codes.
- [Error Codes](./error-codes.md) — complete list of integer return codes.
- [Examples](./examples.md) — annotated end-to-end examples including story replay.
