---
sidebar_position: 2
title: "Client + Performance test"
---

# Client + Performance test (Multi Node)

With your multi-node ChronoLog deployment running, you can now run a client performance test to exercise the system and validate the distributed setup.

> **Prerequisite:** Make sure you have already completed the steps in [Running ChronoLog](./running-chronolog) so that the multi-node ChronoLog deployment is up and running.

## Running a Client Performance Test

### Step 1: Run the Client Performance Test

Run the benchmark client `chrono-bench` in the `chronolog-c1` container, directly from your host machine:

```bash
docker exec chronolog-c1 bash -c 'cd "$CHRONOLOG_HOME" && tools/benchmark/chrono-bench -c conf/default-chrono-client-conf.json -w -a 4096 -b 4096 -s 4096 -n 4096 -t 1 -h 1 -p'
```

The deployment script set `conf/default-chrono-client-conf.json` in that container to the ChronoVisor's address, so the client reaches the distributed deployment. The test writes (`-w`) 4096 Events (`-n 4096`) of 4096 bytes (`-a`, `-s` and `-b` are the minimum, average and maximum payload size) to one Story (`-t 1`) of one Chronicle (`-h 1`), and prints the performance results (`-p`). See [How to Run a Performance Test](../native-linux/performance-test) for how to read them, and run `tools/benchmark/chrono-bench -u` in the container for every option.

## Stopping ChronoLog

Stop all containers:

```bash
docker compose -f dynamic-compose.yaml down
```

Remove volumes (optional):

```bash
docker compose -f dynamic-compose.yaml down -v
```

## What's Next?

With ChronoLog running and your performance test completed, you can:

- **Deploy workloads within containers**.
- **Integrate ChronoLog into your infrastructure**.
- **Customize setups** based on your needs.

If you haven't yet explored the single-node Docker workflow, see [Docker (single node) - Running ChronoLog](../docker-single-node/running-chronolog) for a simpler starting point.
