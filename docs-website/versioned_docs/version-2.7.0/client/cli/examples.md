---
sidebar_position: 3
title: "Examples"
---

# CLI Examples

## 1. Interactive Session

Launch `client_admin` in interactive mode and manage a Chronicle/Story lifecycle step by step.

```bash
client_admin -i
```

Once the prompt appears, enter commands one at a time:

```
-c my_chronicle
-a -s my_chronicle my_story
-w "This is event 1"
-w "This is event 2"
-q -s my_chronicle my_story
-d -s my_chronicle my_story
-d -c my_chronicle
-disconnect
```

Each command prints a status message confirming the result. Use interactive mode when exploring a deployment or debugging Story state.

## 2. Scripted Workload

Create a script file (`scripted_workload.sh`) with the same commands:

```bash
#!/usr/bin/env bash
# scripted_workload.sh — basic lifecycle workload

client_admin -c my_chronicle
client_admin -a -s my_chronicle my_story
client_admin -w "Event 1"
client_admin -w "Event 2"
client_admin -w "Event 3"
client_admin -q -s my_chronicle my_story
client_admin -d -s my_chronicle my_story
client_admin -d -c my_chronicle
```

Run it with the `-f` flag:

```bash
client_admin -f scripted_workload.sh
```

Scripted mode is suitable for CI pipelines, synthetic workload generation, and automated integration testing.

## 3. Performance Testing

Add the `--perf` flag to any scripted run to collect timing metrics:

```bash
client_admin --perf -f scripted_workload.sh
```

Performance metrics are printed to stdout after execution, showing operation latencies and throughput. Use this to baseline your deployment before and after configuration changes.

To test collaborative (shared) Story access across multiple clients, add `--shared_story`:

```bash
client_admin --shared_story -f scripted_workload.sh
```
