---
sidebar_position: 2
title: "How to Run a Performance Test"
---

# How to Run a Performance Test

## Introduction

Welcome to the second tutorial for ChronoLog! In this tutorial, we will run a performance test on the system with **`chrono-bench`**, the benchmark client installed under `tools/benchmark/`. By the end of this guide, you'll be able to run a write test and read the performance metrics it reports.

### Prerequisites

Before starting, ensure that you have completed **Tutorial 1** (First Steps with ChronoLog), where we set up the system and ran the first test. This tutorial assumes that your ChronoLog system is already running.

---

## Step 1: Verify ChronoLog is Running

To check if ChronoLog is running, execute the following command:

```bash
pgrep -fla "chrono-visor|chrono-grapher|chrono-keeper|chrono-player"
```

If this command does not show all four ChronoLog services, it means ChronoLog is not running properly. In that case, follow the instructions in the [First Steps with ChronoLog](./first-steps) tutorial to set up and start the system.

---

## Step 2: Run the Performance Test

Go to the ChronoLog installation directory and start the test:

```bash
cd /home/$USER/chronolog-install/chronolog
tools/benchmark/chrono-bench -c conf/default-chrono-client-conf.json -w -h 1 -t 1 -n 1000 -a 1024 -s 1024 -b 1024 -p
```

### Breakdown of the Command

- `-c conf/default-chrono-client-conf.json`: the client configuration file.
- `-w`: write mode: the test creates Chronicles and Stories, writes Events, and then destroys them.
- `-h 1`: one Chronicle per process.
- `-t 1`: one Story per process.
- `-n 1000`: 1000 Events per process.
- `-a 1024 -s 1024 -b 1024`: the minimum, average and maximum Event payload size in bytes. With different values, the sizes follow a normal distribution between the minimum and the maximum.
- `-p`: report performance metrics at the end.

Run without `mpiexec`, `chrono-bench` is a single process. To run several processes, start it with the `mpiexec` of the Spack environment ChronoLog was built with, for example `/path-to-repo/.spack-env/view/bin/mpiexec -n 4 tools/benchmark/chrono-bench ...`; the counts above are then per process. Run `tools/benchmark/chrono-bench -u` for every option.

---

## Step 3: Analyze the Output

### Parameters

`chrono-bench` first prints the parameters of the run:

```bash
Config file specified: conf/default-chrono-client-conf.json
Number of processes: 1
Chronicle count (#chronicles per proc): 1
Story count (#stories per proc): 1
No event payload file specified, use default/specified separate conf args ...
Min event size (minimum of event payload length following a normal distribution): 1024 bytes
Ave event size (median of event payload length following a normal distribution): 1024 bytes
Max event size (maximum of event payload length following a normal distribution): 1024 bytes
Event count (#events per proc): 1000
Event interval (time in-between event accesses): 0 us
Barrier (hold on next ChronoLog API call until completion of the previous one): false
Shared story (all procs access the same story): false
Durable writes: false
```

### Configuration Details

It then prints the client configuration it loaded:

```bash
===== ChronoLog Client Configuration =====
[PORTAL_CONF]
  protocol: ofi+sockets
  IP: 127.0.0.1
  port: 5555
  provider ID: 55
[QUERY_CONF]
  protocol: ofi+sockets
  IP: 127.0.0.1
  port: 5557
  provider ID: 57
[LOG_CONF]
  ...
[PerformanceTest] Running in write mode.
Connected to server address: ofi+sockets://127.0.0.1:5555@55
```

### Performance Results

Finally, it displays the performance results:

```bash
==================================================================
=================      Performance results:      =================
==================================================================
Total payload written: 1024000 bytes
Connect throughput: 744.056 connections/s
CreateChronicle throughput: 1967.92 creations/s
AcquireStory throughput: 398.614 acquisitions/s
ReleaseStory throughput: 617.501 releases/s
DestroyStory throughput: 2240.08 destructions/s
DestroyChronicle throughput: 3410 destructions/s
Disconnect throughput: 6847.72 disconnections/s
End-to-end (incl. metadata time) bandwidth: 11.5443 MB/s
Record-event (incl. metadata time) bandwidth: 12.2951 MB/s
Record-event (incl. metadata time) throughput: 12006.9 events/s
Accepted writes: 1000 sent, 1000 acknowledged
Accepted ack latency (us): n=1000  min=59.36  mean=78.99  p50=75.98  p90=92.61  p99=119.93  max=657.75
Accepted acknowledged throughput: 12008.09 events/s over 0.08 s
Accepted return code CL_SUCCESS (0): 1000
```

The values depend on the machine. The metadata lines give the rate of each operation, and the bandwidth lines the payload written per second. The `Accepted` lines count the writes the keepers acknowledged, with their latency and return codes. When the keepers run with their [write-ahead log](../../user-guide/architecture/keeper-wal.md) on, add `--durable` to write each Event durably; the same lines then start with `Durable`. With the write-ahead log off, as in this deployment, every durable write returns `CL_ERR_DURABILITY_UNAVAILABLE`.

## Conclusion

Congratulations! You have successfully run a performance test on ChronoLog with `chrono-bench`. Change the counts, the Event sizes or the number of processes to measure the system under different conditions.

If you encountered any issues, check that the system is running and that the configuration file path is right. For additional support, refer to the [First Steps with ChronoLog](./first-steps) tutorial or reach out to the ChronoLog community.

Stay tuned for the next tutorial!
