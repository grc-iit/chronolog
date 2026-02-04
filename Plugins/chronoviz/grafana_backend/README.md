# ChronoLog Grafana Backend Service

FastAPI REST service that wraps the ChronoLog Python client to provide a backend for the Grafana data source plugin.

## Overview

This service provides HTTP endpoints that Grafana can use to:
- Connect to ChronoLog service
- Query events from stories
- Transform ChronoLog events into Grafana-compatible format

## Installation

### Prerequisites

- Python 3.9+
- ChronoLog client library (with Python bindings)

### Install Dependencies

```bash
pip install -r requirements.txt
```

## Configuration

Configure via environment variables (see `env.example`):

| Variable | Default | Description |
|----------|---------|-------------|
| `HOST` | `0.0.0.0` | Service bind address |
| `PORT` | `8080` | Service port |
| `CHRONOLOG_LIB_PATH` | - | Path to ChronoLog Python bindings |
| `CHRONOLOG_PROTOCOL` | `ofi+sockets` | RPC protocol |
| `CHRONOLOG_PORTAL_IP` | `127.0.0.1` | Portal service IP |
| `CHRONOLOG_PORTAL_PORT` | `5555` | Portal service port |
| `CHRONOLOG_PORTAL_PROVIDER_ID` | `55` | Portal service provider ID |
| `CHRONOLOG_QUERY_IP` | `127.0.0.1` | Query service IP |
| `CHRONOLOG_QUERY_PORT` | `5557` | Query service port |
| `CHRONOLOG_QUERY_PROVIDER_ID` | `57` | Query service provider ID |
| `CHRONOLOG_AUTO_CONNECT` | `false` | Auto-connect on startup |

## Running

### Local Development

```bash
export CHRONOLOG_LIB_PATH=~/chronolog-install/chronolog/lib
export PYTHONPATH=$CHRONOLOG_LIB_PATH:$PYTHONPATH
export LD_LIBRARY_PATH=$CHRONOLOG_LIB_PATH:$LD_LIBRARY_PATH

python chronolog_service.py
```

### Using Docker

```bash
docker build -t chronolog-backend .
docker run -p 8080:8080 \
  -v ~/chronolog-install/chronolog/lib:/opt/chronolog/lib:ro \
  -e CHRONOLOG_PORTAL_IP=host.docker.internal \
  chronolog-backend
```

## API Endpoints

### Health Check
- **GET** `/health`
- Returns service status and connection state

### Connection Management
- **POST** `/connect` - Connect to ChronoLog service
  - Accepts optional `ConnectionConfig` or uses environment defaults
  - Returns connection status
- **POST** `/disconnect` - Disconnect from service

### Story Operations
- **POST** `/acquire` - Acquire a story handle
  ```json
  {
    "chronicle_name": "my_chronicle",
    "story_name": "my_story",
    "attrs": {},
    "flags": 1
  }
  ```
- **POST** `/release` - Release a story handle
  ```json
  {
    "chronicle_name": "my_chronicle",
    "story_name": "my_story"
  }
  ```

### Data Queries
- **POST** `/query` - Query events from a story
  - Automatically handles acquire/release lifecycle
  ```json
  {
    "chronicle_name": "my_chronicle",
    "story_name": "my_story",
    "start_time": 1000000000000000000,
    "end_time": 2000000000000000000
  }
  ```
  - Times are in nanoseconds since epoch

### Grafana Integration
- **GET** `/` - Root endpoint (service info)
- **GET** `/health` - Health check with connection status

## Testing

**⚠️ Important: These are integration tests that require a running ChronoLog infrastructure (ChronoVisor, ChronoKeeper, etc.)**

### Prerequisites for Testing

Before running tests, ensure:
1. ChronoLog services are running (ChronoVisor at minimum)
2. Services are accessible at the configured IP/ports
3. Python bindings are available in your environment

### Run All Tests

```bash
./run_tests.sh
```

This script will:
- Install dependencies
- Set up the Python environment
- Run all tests with coverage reporting

### Run Specific Tests

```bash
# Run all tests with verbose output
pytest test_chronolog_service.py -v -s

# Run specific test class
pytest test_chronolog_service.py::TestChronoLogService -v

# Run specific test
pytest test_chronolog_service.py::TestChronoLogService::test_connect_success -v -s
```

### Run with Coverage

**Note**: Coverage collection is incompatible with `--forked` mode. Use the dedicated script:

```bash
./run_tests_with_coverage.sh
```

**Warning**: This runs tests without `--forked` and may cause segfaults due to C++ client state issues. Use only for coverage collection, not for regular testing.

View coverage report at `htmlcov/index.html`.

### Configure Test Connection

Tests use environment variables for connection parameters. Set these to match your ChronoLog deployment:

```bash
export CHRONOLOG_PORTAL_IP="127.0.0.1"
export CHRONOLOG_PORTAL_PORT="5555"
export CHRONOLOG_PORTAL_PROVIDER_ID="55"
export CHRONOLOG_QUERY_IP="127.0.0.1"
export CHRONOLOG_QUERY_PORT="5557"
export CHRONOLOG_QUERY_PROVIDER_ID="57"

# Optional: Specify chronicle/story for query tests
export TEST_CHRONICLE_NAME="my_chronicle"
export TEST_STORY_NAME="my_story"

pytest test_chronolog_service.py -v -s
```

### Test Categories

The test suite includes:

- **Health Check Tests**: Service health and connection status
- **Connection Tests**: Connect, disconnect with real ChronoLog services
- **Query Tests**: Event querying with real data retrieval (handles acquire/release automatically)
- **Validation Tests**: Invalid parameters and edge cases
- **Model Tests**: Request/response model validation

**Note**: Tests run in separate subprocesses using `pytest-forked` to avoid C++ client state issues.

### Viewing Test Output

Tests include logging that shows:
- Connection status and results
- Query results and event counts
- Test lifecycle information

Use the `-s` flag with pytest to see this output:

```bash
pytest test_chronolog_service.py -v -s --forked
```

## Architecture

```
┌─────────────┐
│   Grafana   │
└──────┬──────┘
       │ HTTP/JSON
┌──────▼──────────────────┐
│  chronolog_service.py   │
│     (FastAPI)           │
│  - Async endpoints      │
│  - Thread pool for C++  │
│  - Thread-safe locking  │
└──────┬──────────────────┘
       │ Python bindings
┌──────▼──────────────────┐
│  py_chronolog_client    │
│    (pybind11)           │
└──────┬──────────────────┘
       │ C++ library
┌──────▼──────────────────┐
│  libchronolog_client.so │
└─────────────────────────┘
```

### Threading Model

The service uses `asyncio.to_thread()` to run all C++ client calls in a dedicated thread pool, with `threading.Lock()` to serialize access. This prevents segfaults from concurrent access to the non-thread-safe C++ client.

## Development

### Code Style

Follow PEP 8 style guidelines. Format code with:

```bash
black chronolog_service.py
```

### Adding New Endpoints

1. Add Pydantic models for request/response
2. Implement the endpoint function with proper error handling
3. Add corresponding unit tests
4. Update API documentation

### Logging

The service uses Python's built-in logging. Adjust log level:

```python
logging.basicConfig(level=logging.DEBUG)
```

## Troubleshooting

### Import Error: py_chronolog_client not found

- Ensure `CHRONOLOG_LIB_PATH` is set correctly
- Verify the Python bindings are built
- Check `PYTHONPATH` includes the library path

### Connection Failed

- Verify ChronoLog service is running
- Check IP addresses and ports are correct
- Ensure network connectivity

### Empty Results

- Verify chronicle and story names
- Check time range is correct (nanoseconds since epoch)
- Ensure data exists in the time range
- Verify story is acquired before querying (or use `/query` which handles this automatically)

### Segfaults in Tests

- Ensure tests run with `--forked` flag (default in `run_tests.sh`)
- Each test runs in a separate subprocess to avoid C++ client state issues
- Do not run tests without `--forked` unless collecting coverage (see coverage section)

## License

Apache-2.0

