---
sidebar_position: 2
title: "API Reference"
---

# CLI API Reference

Full command reference for the `client_cli` binary.

## Launching the CLI

```bash
client_cli [-c <config_file>] [-u|--usage]
```

| Flag | Argument | Description |
|---|---|---|
| `-c`, `--config` | `<config_file>` | Path to a ChronoLog client configuration file |
| `-u`, `--usage` | — | Print the usage page and exit |

On startup, `client_cli` connects to the ChronoVisor portal (and, if configured, the ChronoPlayer query service) and enters an interactive REPL on stdin. Each line is parsed as a single command.

## REPL commands

### Chronicle Operations

| Command | Arguments | Description |
|---|---|---|
| `-c` | `<chronicle>` | Create a Chronicle with the given name |
| `-l -c` | — | List all Chronicles visible to this client |
| `-d -c` | `<chronicle>` | Destroy a Chronicle (and all its Stories) |

### Story Operations

| Command | Arguments | Description |
|---|---|---|
| `-a -s` | `<chronicle> <story>` | Acquire (open) a Story in a Chronicle. Creates the Story if it does not exist. |
| `-q -s` | `<chronicle> <story>` | Release the currently acquired Story |
| `-l -s` | `<chronicle>` | List all Stories in a Chronicle |
| `-d -s` | `<chronicle> <story>` | Destroy a Story |

### Event Operations

| Command | Arguments | Description |
|---|---|---|
| `-w` | `<event>` | Write an Event with the given string payload to the currently acquired Story |
| `-r` | `<chronicle> <story> <start> <end>` | Replay events from `[start, end)` and print them to stdout |

### Session

| Command | Arguments | Description |
|---|---|---|
| `-disconnect` | — | Disconnect from the ChronoLog server and exit the REPL |

## Notes

- `-d` is used for both Chronicle and Story destruction; the object type is determined by the subsequent `-c` or `-s` flag.
- `-l` is used for both Chronicle and Story listing in the same way.
- `-w` writes into the most recently acquired Story; `-a -s` must be called first.
- `-r` requires the CLI to be running in WRITER + READER mode (a `ClientQueryServiceConf` is set in the config file).
