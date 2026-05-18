---
sidebar_position: 1
title: "Overview"
---

# CLI Tool Overview

`client_admin` is the command-line tool for interacting with a running ChronoLog deployment. It lets you create and destroy Chronicles, acquire and release Stories, write Events, and run scripted workloads — all without writing application code.

## Invocation Modes

`client_admin` supports two modes of operation:

### Interactive Mode (`-i`)

Launches a REPL-style session where you enter commands one at a time and see results immediately.

```bash
client_admin -i
```

Use interactive mode for:
- Exploring a live deployment
- Debugging Chronicle/Story state
- Running ad-hoc operations during development

### Scripted Mode (`-f`)

Reads commands from a script file and executes them non-interactively.

```bash
client_admin -f scripted_workload.sh
```

Use scripted mode for:
- Automation and CI/CD pipelines
- Synthetic workload generation
- Repeatable performance testing

## Key Capabilities

- **Chronicles** — create and destroy named Chronicle containers
- **Stories** — acquire (open), release, and destroy Stories within a Chronicle
- **Events** — write string-payload Events into an acquired Story
- **Performance metrics** — collect timing data with `--perf`
- **Collaborative access** — test shared Story scenarios with `--shared_story`

See [API Reference](./api) for the full flag reference and [Examples](./examples) for walkthroughs.
