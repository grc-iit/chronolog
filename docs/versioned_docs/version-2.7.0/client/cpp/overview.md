---
sidebar_position: 1
title: "Overview"
---

# C++ Client Overview

The **C++ Client** is ChronoLog's native client library, providing direct integration with the ChronoLog distributed chronicle storage system. It is designed for high-performance applications that require low-latency event logging and replay without serialization overhead.

### Key Features

- No serialization overhead — events are passed as plain `std::string` records.
- Direct memory access to the ChronoLog event pipeline.
- Full API coverage: chronicle and story lifecycle management, event logging, and story replay.
- Configurable structured logging via [spdlog](https://github.com/gabime/spdlog).

### Requirements

- C++17 or later.
- Linked against the ChronoLog client library (`chronolog_client`).
- [libfabric](https://ofiwg.github.io/libfabric/) (required for the Thallium/Margo RPC transport).

### Including the Library

```cpp
#include <chronolog_client.h>       // Client, StoryHandle, Event
#include <ClientConfiguration.h>   // ClientConfiguration, ClientPortalServiceConf, ClientQueryServiceConf
#include <client_errcode.h>         // ClientErrorCode constants
```

### Client Modes

The `Client` class supports two modes depending on which constructor is used:

| Mode | Constructor | Capabilities |
|------|-------------|-------------|
| **WRITER_MODE** | `Client(ClientPortalServiceConf const &)` | Event logging only |
| **WRITER + READER MODE** | `Client(ClientPortalServiceConf const &, ClientQueryServiceConf const &)` | Event logging and story replay |

Use the single-argument constructor when you only need to produce events. Pass both a portal and a query service configuration to also enable `ReplayStory`.

### Configuration

The `ClientConfiguration` class aggregates all configuration structs and can load settings from a JSON file or use built-in defaults.

```cpp
chronolog::ClientConfiguration config;
config.load_from_file("/path/to/client_conf.json"); // optional

chronolog::ClientPortalServiceConf portalConf = config.PORTAL_CONF;
chronolog::ClientQueryServiceConf  queryConf  = config.QUERY_CONF;
```

Default values:

| Struct | Field | Default |
|--------|-------|---------|
| `ClientPortalServiceConf` | `PROTO_CONF` | `"ofi+sockets"` |
| `ClientPortalServiceConf` | `IP` | `"127.0.0.1"` |
| `ClientPortalServiceConf` | `PORT` | `5555` |
| `ClientPortalServiceConf` | `PROVIDER_ID` | `55` |
| `ClientQueryServiceConf` | `PROTO_CONF` | `"ofi+sockets"` |
| `ClientQueryServiceConf` | `IP` | `"127.0.0.1"` |
| `ClientQueryServiceConf` | `PORT` | `5557` |
| `ClientQueryServiceConf` | `PROVIDER_ID` | `57` |

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

```cpp
chronolog::Client client(portalConf, queryConf);
client.Connect();

int flags = 0;
client.CreateChronicle("MyChronicle", {}, flags);

auto [ret, handle] = client.AcquireStory("MyChronicle", "MyStory", {}, flags);
handle->log_event("hello world");

client.ReleaseStory("MyChronicle", "MyStory");
client.Disconnect();
```

### Next Steps

- [API Reference](./api.md) — full method signatures and return codes.
- [Examples](./examples.md) — annotated end-to-end example including story replay.
