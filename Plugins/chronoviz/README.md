# ChronoViz - ChronoLog Grafana Visualization Plugin

ChronoViz is a Grafana data source plugin that enables visualization and analysis of log events stored in ChronoLog distributed log store.

## Overview

ChronoViz consists of three main components:

1. **Grafana Data Source Plugin** (`grafana_plugin/`): TypeScript/React frontend plugin
2. **Python Backend Service** (`grafana_backend/`): FastAPI REST API that wraps ChronoLog client
3. **Docker Compose Setup** (`docker-compose.yml`): Containerized deployment configuration

## Architecture

Request flow: Grafana UI loads the plugin; the plugin sends HTTP requests to the Python backend; the backend uses the C++ client library, which connects to the ChronoLog service.

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐     ┌─────────────┐     ┌──────────────┐
│   Grafana   │────▶│  ChronoViz   │────▶│   Python    │────▶│    C++      │────▶│  ChronoLog   │
│     UI      │     │   Plugin     │     │   Backend   │     │ Client Lib  │     │   Service    │
└─────────────┘     └──────────────┘     └─────────────┘     └─────────────┘     └──────────────┘
```

## Quick Start

### Prerequisites

- ChronoLog service running and accessible
- **Docker and Docker Compose** (default way to run; ChronoLog client library is used from `CHRONOLOG_INSTALL_PATH`, default `~/chronolog-install/chronolog/lib`)
- *Optional for local dev:* Node.js 18+ and Python 3.9+ and ChronoLog client library on the host

### Quick Start (Docker — default)

The start script uses **Docker** by default. No arguments needed.

1. **Set environment variables** (for the backend container and plugin build):
   ```bash
   export CHRONOLOG_INSTALL_PATH=~/chronolog-install
   export CHRONOLOG_PORTAL_IP=127.0.0.1
   export CHRONOLOG_QUERY_IP=127.0.0.1
   ```

2. **Start Grafana and the backend**:
   ```bash
   cd Plugins/chronoviz
   ./scripts/start_grafana_dev.sh
   ```

3. **Access Grafana**:
   - URL: http://localhost:3000
   - Username: `admin`
   - Password: `admin`

The ChronoLog data source is provisioned automatically.

### Local development (no Docker)

To run only the backend on the host and start Grafana yourself:

1. **Build the plugin**:
   ```bash
   cd Plugins/chronoviz
   ./scripts/build_grafana_plugin.sh
   ```

2. **Start the backend** (in a separate terminal):
   ```bash
   cd Plugins/chronoviz
   ./scripts/start_grafana_dev.sh --local
   ```

3. **Configure Grafana** to load the plugin from `grafana_plugin/dist`

## Project Structure

```
chronoviz/
├── grafana_plugin/          # Grafana data source plugin (TypeScript)
│   ├── src/
│   │   ├── module.ts        # Plugin entry point
│   │   ├── datasource.ts    # Data source implementation
│   │   ├── QueryEditor.tsx  # Query builder UI
│   │   ├── ConfigEditor.tsx # Configuration UI
│   │   └── types.ts         # TypeScript types
│   ├── package.json
│   └── README.md
├── grafana_backend/         # Python backend service (FastAPI)
│   ├── chronolog_service.py # Main service implementation
│   ├── requirements.txt
│   ├── Dockerfile
│   └── env.example
├── grafana_provisioning/    # Grafana provisioning configs
│   └── datasources/
│       └── chronolog.yml
├── scripts/
│   ├── build_grafana_plugin.sh
│   └── start_grafana_dev.sh
├── docker-compose.yml
└── README.md (this file)
```

## Configuration

### Backend Service Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CHRONOLOG_INSTALL_PATH` | `~/chronolog-install` | Path to ChronoLog installation |
| `CHRONOLOG_PORTAL_IP` | `127.0.0.1` | Portal service IP |
| `CHRONOLOG_PORTAL_PORT` | `5555` | Portal service port |
| `CHRONOLOG_QUERY_IP` | `127.0.0.1` | Query service IP |
| `CHRONOLOG_QUERY_PORT` | `5557` | Query service port |
| `CHRONOLOG_AUTO_CONNECT` | `false` | Auto-connect on startup |

### Grafana Data Source Settings

- **Backend URL**: URL of the Python backend service (e.g., `http://localhost:8080` or `http://chronolog-backend:8080` in Docker)

## Usage

### Creating a Query

1. Create a dashboard and add a panel
2. Select **ChronoLog** as the data source
3. Enter:
   - **Chronicle Name**: The chronicle to query
   - **Story Name**: The story within the chronicle
   - Use Grafana's time picker to set the query range

### Data Fields

Each event includes:
- **Time**: Event timestamp
- **Log**: Log record content
- **Client ID**: ID of the client that logged the event
- **Index**: Event index

### Template Variables

Create template variables for dynamic queries:
- List chronicles: Query `chronicles` or `*`
- List stories: Query `stories(chronicle_name)`

## API Endpoints

The Python backend exposes:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Root endpoint (service info) |
| `/health` | GET | Health check with connection status |
| `/connect` | POST | Connect to ChronoLog service |
| `/disconnect` | POST | Disconnect from ChronoLog service |
| `/acquire` | POST | Acquire a story handle |
| `/release` | POST | Release a story handle |
| `/query` | POST | Query events (handles acquire/release automatically) |

## Development

### Building the Plugin

```bash
cd grafana_plugin
npm install
npm run build
```

### Running the Backend

**Default (Docker):** From `Plugins/chronoviz`, run `./scripts/start_grafana_dev.sh` with no arguments. Grafana and the backend run in containers.

**Local backend only:** Use the start script with `--local` (creates/uses a venv and sets env for you), or run the backend manually — see [Grafana Backend README](grafana_backend/README.md) for venv and required env vars:

```bash
cd grafana_backend
pip install -r requirements.txt
export CHRONOLOG_LIB_PATH=~/chronolog-install/chronolog/lib
export LD_LIBRARY_PATH=$CHRONOLOG_LIB_PATH:$LD_LIBRARY_PATH
python chronolog_service.py
```

### Running Tests

Run the backend unit tests:

```bash
cd grafana_backend
./run_tests.sh
```

Or manually with pytest:

```bash
pip install -r requirements-test.txt
pytest test_chronolog_service.py -v
```

Generate coverage report:

```bash
cd grafana_backend
./run_tests_with_coverage.sh
```

**Note**: Coverage collection runs without `--forked` and may cause segfaults. Use only for coverage collection, not regular testing.

Coverage report will be available at `htmlcov/index.html`.

## Troubleshooting

### Backend not connecting

1. Verify ChronoLog service is running
2. Check `CHRONOLOG_INSTALL_PATH` is correct
3. Verify network connectivity to ChronoLog service ports

### Plugin not loading

1. Ensure plugin is in Grafana's plugins directory
2. Add `chronolog-datasource` to `allow_loading_unsigned_plugins`
3. Check Grafana logs for errors

### No data returned

1. Test backend: `curl http://localhost:8080/health`
2. Verify chronicle and story names
3. Check time range includes data

## License

Apache-2.0

## Related Documentation

- [Grafana Backend README](grafana_backend/README.md) - Backend setup, env vars, and running locally
- [Grafana Plugin README](grafana_plugin/README.md) - Detailed plugin documentation
- [ChronoLog Documentation](../../README.md) - Main ChronoLog documentation

