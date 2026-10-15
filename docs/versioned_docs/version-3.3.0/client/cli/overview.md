---
sidebar_position: 1
title: "Overview"
---

# CLI Tool Overview

`client_cli` is the command-line tool for interacting with a running ChronoLog deployment. It lets you create and destroy Chronicles, acquire and release Stories, write Events, tail-read the most recent Events from ChronoKeeper memory, and replay events from the archive — all without writing application code.

## Invocation

`client_cli` is launched once with an optional config-file argument and then enters an interactive REPL on stdin:

```bash
client_cli -c /path/to/client_conf.json
```

Each line on stdin is parsed as a single command (see [API Reference](./api)). Scripted workloads are run by piping a file into stdin or via shell redirection:

```bash
client_cli -c /path/to/client_conf.json < scripted_workload.txt
```

The REPL exits when the special `-disconnect` command is read (or stdin is closed).

## Key Capabilities

- **Chronicles** — create, list, and destroy named Chronicle containers
- **Stories** — acquire (open), release, list, and destroy Stories within a Chronicle
- **Events** — write string-payload Events into an acquired Story
- **Tail read** — play back the most recent $N$ Events of the acquired Story directly from ChronoKeeper memory (`-p`); works in writer-only mode
- **Replay** — read events back from a Story over a time range (requires WRITER + READER mode in the config)

See [API Reference](./api) for the full command reference and [Examples](./examples) for walkthroughs.
