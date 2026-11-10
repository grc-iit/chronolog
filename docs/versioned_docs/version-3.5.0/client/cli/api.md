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
| `-p` | `<num_events>` | Tail-read: play back the most recent `<num_events>` Events of the currently acquired Story from ChronoKeeper memory and print them to stdout |
| `-r` | `<chronicle> <story> <start> <end>` | Replay events from `[start, end)` and print them to stdout |

### Session

| Command | Arguments | Description |
|---|---|---|
| `-disconnect` | — | Disconnect from the ChronoLog server and exit the REPL |

## Notes

- `-d` is used for both Chronicle and Story destruction; the object type is determined by the subsequent `-c` or `-s` flag.
- `-l` is used for both Chronicle and Story listing in the same way.
- `-w` writes into the most recently acquired Story; `-a -s` must be called first.
- `-p` reads from the most recently acquired Story, so like `-w` it requires `-a -s` first. This is why it takes only a count, while `-r` takes explicit Chronicle and Story names.
- `-p` does **not** require READER mode. The tail read talks directly to the Story's assigned ChronoKeepers, so a writer-only configuration (`ClientPortalServiceConf` alone) is sufficient — no `ClientQueryServiceConf` and no ChronoPlayer are involved. See [On-Demand Tail Read](../../user-guide/architecture/on-demand-tail-read.md).
- `-p` only sees Events whose chunk has sealed (`chunk_duration + acceptance_window`, ~25–30 s with the shipped defaults). Enable the keeper's `live_tail_read` knob to also read the open, unsealed chunk.
- Requesting more Events than the Story holds is **not** an error: `-p 100` against a Story with 8 retained Events returns all 8 and reports a complete result.
- If Events are evicted from the keeper tail between the tail read's two internal phases, the CLI prints the Events it did retrieve and appends a note naming `CL_ERR_PARTIAL_RESULT`, rather than discarding a usable answer. See [Error Codes](../cpp/error-codes.md).
- `-r` requires the CLI to be running in WRITER + READER mode (a `ClientQueryServiceConf` is set in the config file).
