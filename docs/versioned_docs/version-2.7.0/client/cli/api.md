---
sidebar_position: 2
title: "API Reference"
---

# CLI API Reference

Full flag reference for the `client_admin` binary.

## Syntax

```bash
client_admin [flags] [arguments]
```

## Flags

### Mode Flags

| Flag | Arguments | Description |
|---|---|---|
| `-i` | — | Launch interactive mode (REPL) |
| `-f` | `<script>` | Execute commands from a script file |

### Chronicle Operations

| Flag | Arguments | Description |
|---|---|---|
| `-c` | `<chronicle>` | Create a Chronicle with the given name |
| `-d -c` | `<chronicle>` | Destroy a Chronicle |

### Story Operations

| Flag | Arguments | Description |
|---|---|---|
| `-a -s` | `<chronicle> <story>` | Acquire (open) a Story in a Chronicle |
| `-q -s` | `<chronicle> <story>` | Release a Story in a Chronicle |
| `-d -s` | `<chronicle> <story>` | Destroy a Story in a Chronicle |

### Event Operations

| Flag | Arguments | Description |
|---|---|---|
| `-w` | `<event>` | Write an Event with the given string payload |

### Connection

| Flag | Arguments | Description |
|---|---|---|
| `-disconnect` | — | Disconnect from the ChronoLog server |

### Optional Modifiers

| Flag | Arguments | Description |
|---|---|---|
| `--perf` | — | Collect and report performance metrics for operations |
| `--shared_story` | — | Enable collaborative (shared) Story access for testing |

## Notes

- `-d` is used for both Chronicle and Story destruction; the object type is determined by the subsequent `-c` or `-s` flag.
- `-a -s` must be called before `-w` — Events are written into the currently acquired Story.
- In scripted mode (`-f`), each line of the script file is treated as a separate invocation.
