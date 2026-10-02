---
sidebar_position: 3
title: "Examples"
---

# CLI Examples

## 1. Interactive Session

Launch `client_cli` with a config file and manage a Chronicle/Story lifecycle step by step.

```bash
client_cli -c /etc/chronolog/client_conf.json
```

Once the REPL is running, enter commands one at a time:

```
-c my_chronicle
-l -c
-a -s my_chronicle my_story
-w This is event 1
-w This is event 2
-q -s my_chronicle my_story
-l -s my_chronicle
-d -s my_chronicle my_story
-d -c my_chronicle
-disconnect
```

Each command prints a status message confirming the result. Use this mode when exploring a deployment or debugging Story state.

## 2. Scripted Workload

Save the same commands to a plain text file (`scripted_workload.txt`):

```text
-c my_chronicle
-a -s my_chronicle my_story
-w Event 1
-w Event 2
-w Event 3
-q -s my_chronicle my_story
-d -s my_chronicle my_story
-d -c my_chronicle
-disconnect
```

Pipe it into `client_cli`:

```bash
client_cli -c /etc/chronolog/client_conf.json < scripted_workload.txt
```

Scripted runs are suitable for CI pipelines, synthetic workload generation, and automated integration testing.

## 3. Tail-Reading Recent Events

Use `-p` to play back the most recent $N$ Events of the acquired Story straight from ChronoKeeper memory, without going through ChronoPlayer or the HDF5 archive. Unlike `-r`, this works in writer-only mode.

Because `-p` reads the Story you already hold, it takes only a count:

```
-c my_chronicle
-a -s my_chronicle my_story
-w event one
-w event two
-w event three
-p 2
```

Output lists the newest Events in ascending order:

```text
Playback returned 2 event(s) of the last 2 requested
{Event: 1787092834174918027:9151315546987656785:1881171567:event two}
{Event: 1787092834175021654:9151315546987656785:1881171569:event three}
```

Two behaviours are worth knowing when you use this interactively:

- **Events are not immediately visible.** A tail read serves sealed chunks, so an Event appears roughly `chunk_duration + acceptance_window` after it is written (~25–30 s with the shipped defaults). Writing and immediately running `-p` normally returns nothing; wait out the seal window, or enable the keeper's `live_tail_read` knob to read the open chunk too.
- **Asking for more than exists is fine.** `-p 100` against a Story holding 8 retained Events returns all 8 and reports a complete result — it is not an error, and not a partial result.

If some Events are evicted from the keeper tail while the read is in flight, the CLI still prints what it retrieved and adds a note naming `CL_ERR_PARTIAL_RESULT`, so a truncated answer is never mistaken for a whole one.

## 4. Replaying Events

When the CLI is configured in WRITER + READER mode (the client configuration file includes a `ClientQueryServiceConf` section), use `-r` to replay events from a Story over a time range `[start, end)`:

```
-r my_chronicle my_story 0 18446744073709551615
```

Events are printed to stdout in chronological order. Pair `-r` with `-w` in an interactive session to verify writes end-to-end.
