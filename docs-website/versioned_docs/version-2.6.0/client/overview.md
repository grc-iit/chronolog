---
sidebar_position: 0
title: "Overview"
---

# Client API Overview

The Client API section documents every interface available for interacting with a ChronoLog deployment programmatically. ChronoLog provides three client interfaces — a native C++ library, a Python wrapper, and a command-line tool — so you can choose the one that best fits your workflow.

## Available interfaces

| Interface | Language | Primary use case | Link |
|---|---|---|---|
| C++ Client | C++ | Full API access for high-performance applications | [C++ Client](./cpp/overview.md) |
| Python Client | Python | Scripting, data science, and rapid prototyping | [Python Client](./python/overview.md) |
| CLI Tool | Shell | Admin tasks, quick tests, and scripted workflows | [CLI Tool](./cli/overview.md) |

### C++ Client

The native C++ client library provides full access to the ChronoLog API. Use it when you need maximum throughput, fine-grained control over event layout, or are integrating ChronoLog into an existing C++ or MPI-based application. The library exposes the core `Client`, `StoryHandle`, and `Event` classes along with error codes and logging configuration.

See [C++ Client](./cpp/overview.md) for the complete API reference.

### Python Client

A pybind11-based wrapper that exposes the C++ client API to Python. It is well suited for data-science workflows, prototyping, and any situation where development speed matters more than raw throughput. The Python client supports chronicle creation, story acquisition, event logging, and playback queries.

See [Python Client](./python/overview.md) for setup instructions and the API reference.

### CLI Tool

`client_admin` is a command-line utility for interacting with a running ChronoLog deployment. It supports both interactive and scripted modes, making it useful for ad-hoc administration, quick smoke tests, and automated performance benchmarks.

See [CLI Tool](./cli/overview.md) for the full command reference and usage examples.
