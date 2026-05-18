---
sidebar_position: 4
title: "ChronoViz"
---

# ChronoViz

ChronoViz is a native **Grafana data source plugin** that enables visualization and analysis of log events stored in ChronoLog. It lets you browse chronicles and stories, query events over arbitrary time ranges, and render the results in any Grafana panel — all without writing custom scripts or export pipelines.

## Architecture

ChronoViz is composed of three layers:

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐     ┌─────────────┐     ┌──────────────┐
│   Grafana   │────>│  ChronoViz   │────>│   Python    │────>│    C++      │────>│  ChronoLog   │
│     UI      │     │   Plugin     │     │   Backend   │     │ Client Lib  │     │   Service    │
└─────────────┘     └──────────────┘     └─────────────┘     └─────────────┘     └──────────────┘
```

1. **Grafana Data Source Plugin** — a TypeScript/React frontend that integrates with the Grafana query editor and panel system.
2. **Python Backend Service** — a FastAPI REST API that wraps the ChronoLog C++ client library, translating HTTP requests into native client calls.
3. **ChronoLog C++ Client Library** — the same client library used by other plugins, connecting directly to the ChronoLog cluster.

The Grafana UI loads the plugin; the plugin sends HTTP requests to the Python backend; the backend uses the C++ client library to communicate with the running ChronoLog service.

## Prerequisites

- A running ChronoLog service
- Docker and Docker Compose (recommended) **or** Node.js 18+ and Python 3.9+ for local development
- ChronoLog client library installed (default path: `~/chronolog-install/chronolog/lib`)

## Quick Start

### Docker (recommended)

1. **Set environment variables:**

   ```bash
   export CHRONOLOG_INSTALL_PATH=~/chronolog-install
   export CHRONOLOG_PORTAL_IP=127.0.0.1
   export CHRONOLOG_QUERY_IP=127.0.0.1
   ```

2. **Start Grafana and the backend:**

   ```bash
   cd Plugins/chronoviz
   ./scripts/start_grafana_dev.sh
   ```

3. **Open Grafana** at `http://localhost:<GRAFANA_PORT>` (default [http://localhost:3000](http://localhost:3000), credentials: `admin` / `admin`).

The ChronoLog data source is provisioned automatically — no manual configuration is needed.

### Local Development (no Docker)

1. **Build the plugin:**

   ```bash
   cd Plugins/chronoviz
   ./scripts/build_grafana_plugin.sh
   ```

2. **Start the backend** (in a separate terminal):

   ```bash
   cd Plugins/chronoviz
   ./scripts/start_grafana_dev.sh --local
   ```

3. **Configure Grafana** to load the plugin from `grafana_plugin/dist`.

## Configuration

### Backend Environment Variables

| Variable | Default | Description |
|---|---|---|
| `CHRONOLOG_INSTALL_PATH` | `~/chronolog-install` | Path to ChronoLog installation |
| `CHRONOLOG_PORTAL_IP` | `127.0.0.1` | Portal service IP address |
| `CHRONOLOG_PORTAL_PORT` | `5555` | Portal service port |
| `CHRONOLOG_QUERY_IP` | `127.0.0.1` | Query service IP address |
| `CHRONOLOG_QUERY_PORT` | `5557` | Query service port |
| `CHRONOLOG_AUTO_CONNECT` | `false` | Automatically connect to ChronoLog on startup |
| `CHRONOLOG_BACKEND_SERVICE_PORT` | `8080` | Port the Python backend listens on |
| `GRAFANA_PORT` | `3000` | Port Grafana is exposed on |

### Grafana Data Source Settings

When adding or editing the ChronoLog data source inside Grafana, the only required setting is:

- **Backend URL** — URL of the Python backend service (e.g., `http://localhost:8080` or `http://chronolog-backend:8080` when using Docker). The port must match `CHRONOLOG_BACKEND_SERVICE_PORT`.

## Usage

### Creating a Query

1. Create a new dashboard and add a panel.
2. Select **ChronoLog** as the data source.
3. Fill in the query fields:
   - **Chronicle Name** — the chronicle to query.
   - **Story Name** — the story within the chronicle.
   - Use Grafana's time picker to set the query range.

### Data Fields

Each returned event includes:

| Field | Description |
|---|---|
| **Time** | Event timestamp |
| **Log** | Log record content |
| **Client ID** | ID of the client that recorded the event |
| **Index** | Event index within the story |

### Template Variables

Grafana template variables allow dynamic, dashboard-wide filtering:

- **List chronicles** — set the query to `chronicles` or `*`.
- **List stories for a chronicle** — set the query to `stories(<chronicle_name>)`.

## API Reference

The Python backend exposes the following REST endpoints:

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | Service info |
| `/health` | GET | Health check with connection status |
| `/connect` | POST | Connect to the ChronoLog service |
| `/disconnect` | POST | Disconnect from the ChronoLog service |
| `/acquire` | POST | Acquire a story handle |
| `/release` | POST | Release a story handle |
| `/query` | POST | Query events (acquires and releases the handle automatically) |

## Troubleshooting

### Backend not connecting

1. Verify the ChronoLog service is running and reachable.
2. Confirm `CHRONOLOG_INSTALL_PATH` points to a valid installation.
3. Check network connectivity to the portal and query service ports.

### Plugin not loading in Grafana

1. Ensure the built plugin is in Grafana's configured plugins directory.
2. Add `chronolog-datasource` to `allow_loading_unsigned_plugins` in `grafana.ini`.
3. Check Grafana server logs for plugin load errors.

### No data returned

1. Test backend health: `curl http://localhost:${CHRONOLOG_BACKEND_SERVICE_PORT:-8080}/health`
2. Verify the chronicle and story names match existing data.
3. Confirm the selected time range overlaps with recorded events.
