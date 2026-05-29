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

## 3. Replaying Events

When the CLI is configured in WRITER + READER mode (the client configuration file includes a `ClientQueryServiceConf` section), use `-r` to replay events from a Story over a time range `[start, end)`:

```
-r my_chronicle my_story 0 18446744073709551615
```

Events are printed to stdout in chronological order. Pair `-r` with `-w` in an interactive session to verify writes end-to-end.
