---
sidebar_position: 3
title: "ChronoStream"
---

# ChronoStream

ChronoStream is a **streaming telemetry plugin** for ChronoLog. It reads log events from ChronoLog stories in real time and forwards them to a visualization backend (currently InfluxDB + Grafana), giving operators a live dashboard of system metrics without modifying the producing application.

## Architecture

ChronoStream uses two cooperating client applications and an external time-series stack:

```
┌────────────────┐     ┌──────────────┐     ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  client_writer │────>│  ChronoLog   │────>│client_reader  │────>│   InfluxDB   │────>│   Grafana    │
│  (producer)    │     │  Cluster     │     │_stream_influx │     │              │     │  Dashboard   │
└────────────────┘     └──────────────┘     └──────────────┘     └──────────────┘     └──────────────┘
```

1. **`client_writer`** — collects host metrics (CPU, memory, network) from `/proc` and writes them as log events into ChronoLog stories.
2. **`client_reader_stream_influx`** — polls ChronoLog stories on a configurable interval, converts events to InfluxDB line protocol, and pushes them to InfluxDB in batches. It maintains a per-story cursor to avoid re-reading events.
3. **InfluxDB + Grafana** — InfluxDB stores the time-series data; a pre-provisioned Grafana dashboard renders it with filtering by chronicle, story, and time range.

### Key Components

| Component | Path | Description |
|---|---|---|
| `client_writer` | `ClientScripts/client_writer.cpp` | Reads CPU/memory/network from `/proc`, writes to ChronoLog at a configurable interval |
| `client_reader_stream_influx` | `ClientScripts/client_reader_stream_influx.cpp` | Replays stories from ChronoLog and streams events to InfluxDB via HTTP |
| `InfluxDBSink` | `StreamingScripts/InfluxDBSink.{h,cpp}` | HTTP sink that POSTs line protocol to InfluxDB v2 with token auth, timeouts, and retries |
| `StreamSink` | `StreamingScripts/StreamSink.h` | Generic interface for streaming backends |
| `Transform` | `StreamingScripts/Transform.{h,cpp}` | Converts ChronoLog events into `TelemetryBatch` line protocol for InfluxDB |

## Prerequisites

- A running ChronoLog service (all four processes: Visor, Grapher, Player, Keeper)
- Docker and Docker Compose (for Grafana and InfluxDB)
- ChronoLog client library installed
- Spack environment activated (for building)

## Setup

### 1. Environment Variables

```bash
cd <ChronoLog repo>
source ~/<spack-directory>/spack/share/spack/setup-env.sh
spack env activate .

export VM_IP=$(hostname -I | awk '{print $1}')
export INSTALL_PREFIX=~/chronolog-install
export INSTALL_DIR="$INSTALL_PREFIX/chronolog"
export SRC_ROOT=~/Desktop/ChronoLog
export LD_LIBRARY_PATH="$INSTALL_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export INFLUX_ORG=chronolog
export INFLUX_BUCKET=telemetry
export INFLUX_TOKEN=chronolog-dev-token-123
export INFLUX_URL="http://$VM_IP:8086/api/v2/write?org=$INFLUX_ORG&bucket=$INFLUX_BUCKET&precision=ns"
```

:::note
After setting `VM_IP`, update the IP address in the InfluxDB datasource provisioning file at `Plugins/chronostream/GrafanaInfluxSetup/provisioning/datasources/datasource.yml`.
:::

### 2. Build and Install ChronoLog

```bash
cd $SRC_ROOT/tools/deploy/ChronoLog
./local_single_user_deploy.sh -b -I $INSTALL_PREFIX
./local_single_user_deploy.sh -i -I $INSTALL_PREFIX
```

### 3. Launch Grafana and InfluxDB

```bash
cd Plugins/chronostream/GrafanaInfluxSetup/
docker compose --env-file .env up -d
```

Verify the containers are running:

```bash
docker ps
```

You should see both `grafana` (port 3000) and `influxdb` (port 8086) containers.

To stop and clean up:

```bash
docker compose down
# Optional: delete the volume for a full refresh
docker volume rm $(docker volume ls -q | grep grafana-data)
```

### 4. Start ChronoLog

```bash
./local_single_user_deploy.sh -d -w $INSTALL_DIR
```

Confirm all four processes are running:

```bash
ps -ef | grep chrono
```

You should see `chrono-visor`, `chrono-grapher`, `chrono-player`, and `chrono-keeper`.

To stop ChronoLog:

```bash
./local_single_user_deploy.sh -s -w $INSTALL_DIR
```

## Usage

### Writing Metrics

Start the client writer to collect and log host metrics:

```bash
export CHRONICLE=grafana_chronicle
$INSTALL_DIR/bin/client_writer \
  -c $INSTALL_DIR/conf/default-chrono-client-conf.json \
  -r "$CHRONICLE" \
  -d 120 \
  -i 5
```

| Flag | Description |
|---|---|
| `-c` | Path to client configuration file |
| `-r` | Chronicle name |
| `-d` | Duration in seconds |
| `-i` | Sampling interval in seconds |

The writer creates three stories under the chronicle: `cpu_usage`, `memory_usage`, and `network_usage`.

### Streaming to Grafana

After the writer has been running for 30-45 seconds, start the reader to stream events through InfluxDB to Grafana:

```bash
$INSTALL_DIR/bin/client_reader_stream_influx \
  -c $INSTALL_DIR/conf/default-chrono-client-conf.json \
  -r "$CHRONICLE" \
  -s cpu_usage,memory_usage,network_usage \
  --window-sec 300 \
  --poll-interval-sec 5 \
  --influx-url "$INFLUX_URL" \
  --influx-token "$INFLUX_TOKEN"
```

| Flag | Description |
|---|---|
| `-c` | Path to client configuration file |
| `-r` | Chronicle name |
| `-s` | Comma-separated list of story names to stream |
| `--window-sec` | Replay window size in seconds |
| `--poll-interval-sec` | Polling interval in seconds |
| `--influx-url` | InfluxDB v2 write endpoint URL |
| `--influx-token` | InfluxDB authentication token |

To stop the reader, press `Ctrl+Z` and then terminate the process:

```bash
ps -ef | grep client_reader_stream_influx
kill -KILL <pid>
```

### Viewing Dashboards

1. Open Grafana at `http://<VM_IP>:3000/` (default credentials: `admin` / `admin`).
2. Navigate to **Dashboards > ChronoLog > ChronoLog Telemetry**.
3. Use the dashboard filters to select a chronicle name, story name, or time range, then click refresh.

## Roadmap

:::info
The items below describe planned improvements to ChronoStream.
:::

- **Direct data source (v2)** — Replace InfluxDB with a native ChronoLog data source in Grafana (see [ChronoViz](./chronoviz.md)), removing the dependency on a third-party time-series store.
- **Automatic push (v3)** — Stream events directly from ChronoLog to Grafana as they are written, eliminating the need for the explicit reader application.
