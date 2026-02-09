# ChronoLog Grafana Data Source Plugin

A Grafana data source plugin for visualizing log events from the ChronoLog distributed log store.

## Overview

This plugin enables Grafana to query and visualize data from ChronoLog chronicles and stories. It consists of two components:

1. **Grafana Plugin (TypeScript/React)**: Frontend data source plugin that runs in Grafana
2. **Python Backend Service (FastAPI)**: REST API that wraps the ChronoLog C++ client library

## Architecture

```
┌─────────────────┐     ┌─────────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  Grafana UI     │────▶│  ChronoLog Plugin   │────▶│  Python Backend  │────▶│  ChronoLog      │
│  (Dashboard)    │     │  (TypeScript)       │     │  (FastAPI)       │     │  Service        │
└─────────────────┘     └─────────────────────┘     └──────────────────┘     └─────────────────┘
                               │                           │
                               │                           │ Uses py_chronolog_client
                               │                           │ (pybind11 bindings)
                               │                           ▼
                               │                    ┌──────────────────┐
                               │                    │  C++ Client Lib  │
                               │                    │  (libchronolog   │
                               └───────HTTP─────────│   _client.so)    │
                                                    └──────────────────┘
```

## Prerequisites

- **ChronoLog**: ChronoLog service running and accessible
- **ChronoLog Client Library**: Built and installed (typically at `~/chronolog-install/chronolog/lib`)
- **Python 3.9+**: For the backend service
- **Node.js 18+**: For building the Grafana plugin
- **Docker & Docker Compose** (optional): For containerized deployment

## Quick Start

### Option 1: Docker Compose (Recommended)

1. **Set environment variables**:
   ```bash
   export CHRONOLOG_INSTALL_PATH=~/chronolog-install
   export CHRONOLOG_PORTAL_IP=127.0.0.1
   export CHRONOLOG_PORTAL_PORT=5555
   export CHRONOLOG_QUERY_IP=127.0.0.1
   export CHRONOLOG_QUERY_PORT=5557
   ```

2. **Build and start services** (script uses Docker by default):
   ```bash
   cd /path/to/ChronoLog/Plugins/chronoviz
   ./scripts/start_grafana_dev.sh
   ```

3. **Access Grafana**:
   - URL: http://localhost:3000
   - Username: `admin`
   - Password: `admin`

### Option 2: Local Development

1. **Build the plugin**:
   ```bash
   cd grafana_plugin
   npm install
   npm run build
   ```

2. **Start the Python backend**:
   ```bash
   cd grafana_backend
   pip install -r requirements.txt
   
   # Set library path
   export CHRONOLOG_LIB_PATH=~/chronolog-install/chronolog/lib
   export PYTHONPATH=$CHRONOLOG_LIB_PATH:$PYTHONPATH
   export LD_LIBRARY_PATH=$CHRONOLOG_LIB_PATH:$LD_LIBRARY_PATH
   
   python chronolog_service.py
   ```

3. **Configure Grafana**:
   - Copy `grafana_plugin/dist` to your Grafana plugins directory
   - Add to `grafana.ini`:
     ```ini
     [plugins]
     allow_loading_unsigned_plugins = chronolog-datasource
     ```
   - Restart Grafana

## Configuration

### Backend Service Configuration

The Python backend can be configured via environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `HOST` | `0.0.0.0` | Service bind address |
| `PORT` | `8080` | Service port |
| `CHRONOLOG_LIB_PATH` | - | Path to ChronoLog library |
| `CHRONOLOG_PROTOCOL` | `ofi+sockets` | RPC protocol |
| `CHRONOLOG_PORTAL_IP` | `127.0.0.1` | Portal service IP |
| `CHRONOLOG_PORTAL_PORT` | `5555` | Portal service port |
| `CHRONOLOG_PORTAL_PROVIDER_ID` | `55` | Portal provider ID |
| `CHRONOLOG_QUERY_IP` | `127.0.0.1` | Query service IP |
| `CHRONOLOG_QUERY_PORT` | `5557` | Query service port |
| `CHRONOLOG_QUERY_PROVIDER_ID` | `57` | Query provider ID |
| `CHRONOLOG_AUTO_CONNECT` | `false` | Auto-connect on startup |

### Grafana Data Source Configuration

1. Go to **Configuration > Data Sources** in Grafana
2. Click **Add data source**
3. Search for **ChronoLog**
4. Configure the **Backend URL** (e.g., `http://localhost:8080`)
5. Click **Save & Test**

## Usage

### Creating a Dashboard

1. Create a new dashboard in Grafana
2. Add a panel
3. Select **ChronoLog** as the data source
4. Enter the **Chronicle Name** and **Story Name**
5. Use Grafana's time picker to select the query range

### Query Fields

| Field | Required | Description |
|-------|----------|-------------|
| Chronicle Name | Yes | Name of the chronicle to query |
| Story Name | Yes | Name of the story within the chronicle |

### Data Fields Returned

Each event includes:

| Field | Type | Description |
|-------|------|-------------|
| Time | timestamp | Event timestamp |
| Log | string | Log record content |
| Client ID | number | ID of the client that logged the event |
| Index | number | Event index |

### Template Variables

You can use template variables for dynamic queries:

- **List chronicles**: Query `chronicles` or `*`
- **List stories**: Query `stories(chronicle_name)`

## API Endpoints

The Python backend exposes the following endpoints:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/health` | GET | Health check |
| `/connect` | POST | Connect to ChronoLog service |
| `/disconnect` | POST | Disconnect from service |
| `/chronicles` | GET | List all chronicles |
| `/stories/{chronicle}` | GET | List stories in a chronicle |
| `/query` | POST | Query events from a story |

### Query Example

```bash
curl -X POST http://localhost:8080/query \
  -H "Content-Type: application/json" \
  -d '{
    "chronicle_name": "my_chronicle",
    "story_name": "my_story",
    "start_time": 1704067200000000000,
    "end_time": 1704153600000000000
  }'
```

## Development

### Building the Plugin

```bash
cd grafana_plugin

# Install dependencies
npm install

# Development build (with watch)
npm run dev

# Production build
npm run build

# Run linting
npm run lint
```

### Backend Development

```bash
cd grafana_backend

# Create virtual environment
python -m venv venv
source venv/bin/activate

# Install dependencies
pip install -r requirements.txt

# Run with auto-reload
python chronolog_service.py
```

## Troubleshooting

### Backend not connecting to ChronoLog

1. Verify ChronoLog service is running
2. Check `CHRONOLOG_LIB_PATH` points to the correct library directory
3. Verify network connectivity to ChronoLog service ports

### Plugin not loading in Grafana

1. Ensure `allow_loading_unsigned_plugins = chronolog-datasource` is set
2. Check Grafana logs for plugin loading errors
3. Verify the plugin is in the correct plugins directory

### No data returned

1. Test the backend directly: `curl http://localhost:8080/health`
2. Verify chronicle and story names are correct
3. Check the time range includes data

## License

Apache-2.0

