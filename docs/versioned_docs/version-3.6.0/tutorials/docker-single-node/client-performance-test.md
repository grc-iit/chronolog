---
sidebar_position: 2
title: "Client + Performance test"
---

# Client + Performance test (Single Node)

With the ChronoLog environment set up and all components deployed, it's time to test the system by running a client and analyzing its performance.

> **Prerequisite:** Make sure you have already completed the steps in [Running ChronoLog](./running-chronolog) so that ChronoLog is up and running inside the Docker container.

## Running a ChronoLog Client + Performance Test

### Step 1: Execute the ChronoLog Client

Inside the container, in the ChronoLog installation directory (`$CHRONOLOG_HOME`, the working directory), run the benchmark client `chrono-bench`:

```bash
tools/benchmark/chrono-bench -c conf/default-chrono-client-conf.json \
    -w -h 2 -t 2 -a 100 -s 100 -b 100 -n 100 -p
```

This command:

- Launches **`chrono-bench`** as one client process.
- Writes (`-w`) to **2 Chronicles** (`-h 2`) with **2 Stories** each (`-t 2`).
- Writes **100 Events** (`-n 100`) of **100 bytes** each (`-a`, `-s` and `-b` are the minimum, average and maximum payload size).
- Prints the **performance results** at the end (`-p`).

Once executed, the client will create **Chronicles**, **Stories**, and **Events**, simulating real workloads to evaluate system performance. See [How to Run a Performance Test](../native-linux/performance-test) for how to read the results, and run `tools/benchmark/chrono-bench -u` for every option.

## Stopping the ChronoLog Container

Once you're done using ChronoLog, **exit the container**:

```bash
exit
```

Then **stop the running instance** (if it wasn't started with `--rm`):

```bash
docker stop chronolog-instance
```

And if you want to **remove** it completely:

```bash
docker rm chronolog-instance
```

## What's Next?

Now that you have ChronoLog running in Docker and have executed a performance test, you're ready to:

- Explore different **ChronoLog deployment options** by checking the [Deployment](../../user-guide/deployment/single-node) guide.
- Discover the potential of **ChronoLog Clients**: [Client Examples](../../client/cpp/examples) (Basic Client, Interactive Client, Scripted Client & Python Client).
- Start **building your own applications** on top of ChronoLog.

If you're interested in scaling out, you can move on to the multi-node setup in [Docker (multi node)](../docker-multi-node/running-chronolog).
