---
sidebar_position: 2
title: "How to Run a Performance Test"
---

# How to Run a Performance Test

## Introduction

Welcome to the second tutorial for ChronoLog! In this tutorial, we will run a performance test on the system using the **Scripted Client Admin** tool. By the end of this guide, you'll be able to execute a scripted test and analyze the system's performance metrics.

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

## Step 2: Prepare the Payload file (Optional)

We will use **Scripted Client Admin** to run a performance test. To do this, create a payload file (e.g., `payload.txt`) containing a set of messages to be sent during the test.

Example:

```
event 1
event 2
event 3
event 4
event 5
event 6
event 7
event 8
event 9
event 10
```

---

## Step 3: Run the Performance Test

Execute the following command to start the performance test:

```bash
./chrono-client_admin -f payload.txt -c /home/$USER/chronolog-install/chronolog/conf/default-chrono-client-conf.json --perf
```

### Breakdown of the Command

- `-f payload.txt`: Specifies the payload file containing all event payloads, one event per line.
- `-c /home/$USER/chronolog-install/chronolog/conf/default-chrono-client-conf.json`: Points to the system's configuration file for clients.
- `--perf`: Enables performance testing mode.

---

## Step 4: Analyze the Output

Once you run the test, the result will display several sections, including system status, configuration details, and performance results.

### Parameters

After executing the command, you will see the system's operational status:

```bash
Config file specified: /home/$USER/chronolog-install/chronolog/conf/default-chrono-conf.json
Interactive mode: off
Chronicle count: 1
Story count: 1
Event input file specified: scripted.sh
Barrier: false
Shared story: false
[ConfigurationManager] Loading configuration from file: /home/$USER/chronolog-install/chronolog/conf/default-chrono-conf.json
```

### Configuration Details

The system will then output its configuration settings:

```bash
******** Start of configuration output ********
CLOCK_CONF: CLOCKSOURCE_TYPE: CPP_STYLE, DRIFT_CAL_SLEEP_SEC: 10, DRIFT_CAL_SLEEP_NSEC: 0
AUTH_CONF: AUTH_TYPE: RBAC, MODULE_PATH: /path/to/auth_module
...
CLIENT_CONF: [VISOR_CLIENT_PORTAL_SERVICE_CONF: [RPC_CONF: [PROTO_CONF: ofi+sockets, IP: 127.0.0.1, BASE_PORT: 5555, SERVICE_PROVIDER_ID: 55, PORTS: ]], CLIENT_LOG_CONF:[TYPE: file, FILE: chrono_client.log, LEVEL: DEBUG, NAME: ChronoClient, LOGFILESIZE: 1048576, LOGFILENUM: 3, FLUSH LEVEL: WARN]]
******** End of configuration output ********
```

### Performance Results

Finally, the system will display the performance test results:

```bash
======================================
======== Performance results: ========
======================================
Total payload written: 1638400000 bytes
Connect throughput: 844.619 connections/s
CreateChronicle throughput: 4093.63 creations/s
AcquireStory throughput: 554.781 acquisitions/s
ReleaseStory throughput: 247.978 releases/s
DestroyStory throughput: 1119.15 destructions/s
DestroyChronicle throughput: 6111.26 destructions/s
Disconnect throughput: 217.764 disconnections/s
End-to-end (incl. metadata time) bandwidth: 209.729 MB/s
Record-event (incl. metadata time) bandwidth: 26.8209 MB/s
Record-event (incl. metadata time) throughput: 0.00654807 events/s
```

These values provide insight into how efficiently ChronoLog is handling operations.

## Conclusion

Congratulations! You have successfully run a performance test on ChronoLog using the **Scripted Client Admin** tool. This process allows you to measure system throughput and analyze performance under different conditions.

If you encountered any issues, double-check that the system is running and verify your script file. For additional support, refer to the [First Steps with ChronoLog](./first-steps) tutorial or reach out to the ChronoLog community.

Stay tuned for the next tutorial!
