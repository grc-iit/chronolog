# Test Suite Summary for ChronoLog Grafana Backend

## Overview

Comprehensive integration test suite for `chronolog_service.py` using pytest and httpx.AsyncClient with ASGITransport. Tests use the real ChronoLog Python client and require ChronoLog services (ChronoVisor, ChronoKeeper) to be running.

## Test Files

- **`test_chronolog_service.py`**: Main integration test suite (480 lines)
- **`pytest.ini`**: Pytest configuration (uses `--forked` for subprocess isolation)
- **`requirements-test.txt`**: Test dependencies (pytest, pytest-forked, httpx, coverage)
- **`run_tests.sh`**: Test runner script (with `--forked` for stable execution)
- **`run_tests_with_coverage.sh`**: Coverage collection script (without `--forked`, may cause segfaults)
- **`.coveragerc`**: Coverage configuration file

## Test Coverage

### Test Classes

#### 1. `TestChronoLogService` (6 active test methods, 3 commented out)

**Tests Without Connection Required:**
- `test_health_check_not_connected()` - Health check when not connected to ChronoLog
- `test_root_endpoint()` - Root endpoint returns service info

**Connection/Disconnection Tests:**
- `test_connect_disconnect_success()` - ⚠️ COMMENTED OUT: Successful connection and disconnection lifecycle

**Acquire/Release Tests:**
- `test_acquire_release_success()` - ⚠️ COMMENTED OUT: Successful acquire/release of a story

**Query Tests:**
- `test_query_story_success()` - Successful story query with real data (connect -> acquire -> release -> query -> disconnect)
- `test_query_not_connected()` - Query endpoint when not connected (expects 503 status)
- `test_multiple_queries_same_connection()` - Multiple queries using the same connection (tests acquire/release pairs)

**Additional Endpoint Tests:**
- `test_search_endpoint_not_connected()` - ⚠️ COMMENTED OUT: Search endpoint when not connected

**Validation Tests:**
- `test_query_validation_missing_fields()` - Query endpoint with missing required fields (expects 422 validation error)

#### 2. `TestConnectionConfig` (2 test methods)

- `test_connection_config_defaults()` - Default configuration values
- `test_connection_config_custom_values()` - Custom configuration values

#### 3. `TestRequestModels` (2 test methods)

- `test_query_request_model()` - QueryRequest model validation
- `test_event_response_model()` - EventResponse model validation

**Total Active Tests: 10**

## Test Strategies

### Integration Testing Strategy

Tests use the **real ChronoLog Python client** (not mocks):
- Tests require ChronoLog services (ChronoVisor, ChronoKeeper) to be running
- Uses `httpx.AsyncClient` with `ASGITransport` instead of FastAPI's `TestClient`
- Wrapped in `SyncClientWrapper` to run async calls synchronously in a single event loop
- Each test runs in a separate subprocess using `pytest-forked` to avoid C++ client state issues

### Test Fixtures

- `setup()` fixture (autouse=True) runs before/after each test:
  - **Before test:**
    - Suppresses asyncio debug messages
    - Logs test name
    - Fully disconnects and cleans up any existing ChronoLog client
    - Forces garbage collection and small delay to ensure C++ object destruction
    - Creates `httpx.AsyncClient` with `ASGITransport` wrapped in `SyncClientWrapper`
    - Sets up connection parameters from environment variables
  - **After test:**
    - Closes httpx client
    - Fully disconnects and cleans up ChronoLog client
    - Forces garbage collection and delay for C++ cleanup

### Test Lifecycle

Each test follows proper ChronoLog lifecycle:
1. **Connection Tests**: `connect -> verify -> disconnect`
2. **Acquire/Release Tests**: `connect -> acquire -> release -> disconnect`
3. **Query Tests**: `connect -> acquire -> release -> query -> disconnect` (or `connect -> query -> disconnect` if backend handles acquire/release)
4. **Multiple Queries**: `connect -> query1 -> query2 -> query3 -> disconnect`

### Edge Cases Covered

1. **Connection States**: Tests when not connected (503 errors)
2. **Error Handling**: Validation errors (422), service unavailable (503)
3. **Query Validation**: Missing required fields
4. **Multiple Operations**: Multiple queries on same connection
5. **C++ Client State**: Proper cleanup between tests to avoid segfaults

### Subprocess Isolation

Tests use `pytest-forked` to run each test in a separate subprocess:
- **Why**: The C++ ChronoLog client is not thread-safe and can cause segfaults when shared between tests
- **Benefit**: Complete isolation ensures no shared state between tests
- **Trade-off**: Coverage collection is disabled when using `--forked` (see Coverage section)

## Running Tests

### Prerequisites

**⚠️ IMPORTANT**: Tests require ChronoLog services to be running:
- ChronoVisor (portal service)
- ChronoKeeper (query service)

Set environment variables for connection (or use defaults):
```bash
export CHRONOLOG_PORTAL_IP=127.0.0.1
export CHRONOLOG_PORTAL_PORT=5555
export CHRONOLOG_PORTAL_PROVIDER_ID=55
export CHRONOLOG_QUERY_IP=127.0.0.1
export CHRONOLOG_QUERY_PORT=5557
export CHRONOLOG_QUERY_PROVIDER_ID=57

# Optional: Test data configuration
export TEST_CHRONICLE_NAME=kfeng-EVO-X2
export TEST_STORY_NAME=cpu_usage
```

### Quick Start

```bash
cd Plugins/chronoviz/grafana_backend
./run_tests.sh
```

This script:
- Creates/activates virtual environment
- Installs dependencies
- Sets up library paths
- Runs tests with `--forked` (each test in separate subprocess)
- **Note**: Coverage collection is disabled with `--forked`

### Coverage Collection

To collect coverage (may cause segfaults due to C++ client state):
```bash
./run_tests_with_coverage.sh
```

**Warning**: This runs tests WITHOUT `--forked`, which may cause segfaults due to C++ client state issues between tests. Use only for coverage collection, not for regular testing.

### Manual Execution

```bash
# Install test dependencies
pip install -r requirements-test.txt

# Set library paths
export CHRONOLOG_LIB_PATH="$HOME/chronolog-install/chronolog/lib"
export PYTHONPATH="$CHRONOLOG_LIB_PATH:$PYTHONPATH"
export LD_LIBRARY_PATH="$CHRONOLOG_LIB_PATH:$LD_LIBRARY_PATH"

# Run all tests with verbose output (with --forked for stability)
pytest test_chronolog_service.py -v --forked

# Run specific test class
pytest test_chronolog_service.py::TestChronoLogService -v --forked

# Run specific test method
pytest test_chronolog_service.py::TestChronoLogService::test_query_story_success -v --forked

# Run with coverage (WARNING: without --forked, may segfault)
coverage run --source=chronolog_service -m pytest test_chronolog_service.py -v
coverage report -m
coverage html

# Run and stop at first failure
pytest test_chronolog_service.py -x --forked
```

## Coverage Goals

The test suite aims for:
- **Line Coverage**: >90%
- **Branch Coverage**: >85%
- **Function Coverage**: 100% of public functions

### Coverage Collection Limitations

**⚠️ Important**: Coverage collection is incompatible with `pytest-forked`:
- `pytest-forked` runs each test in a separate subprocess
- `pytest-cov` cannot collect coverage data from forked subprocesses
- **Solution**: Use `run_tests_with_coverage.sh` which runs tests WITHOUT `--forked`
- **Trade-off**: Tests may segfault without `--forked` due to C++ client state issues

### Key Areas Covered

- ✅ Health check endpoint
- ✅ Root endpoint
- ✅ Query endpoint (with and without connection)
- ✅ Query validation
- ✅ Connection configuration models
- ✅ Request/response models
- ⚠️ Connection/disconnection lifecycle (commented out)
- ⚠️ Acquire/release operations (commented out)

## Expected Test Results

All active tests should pass with output similar to:

```
============================================================
Running test: test_health_check_not_connected
============================================================
test_chronolog_service.py::TestChronoLogService::test_health_check_not_connected PASSED

============================================================
Running test: test_root_endpoint
============================================================
test_chronolog_service.py::TestChronoLogService::test_root_endpoint PASSED

============================================================
Running test: test_query_story_success
============================================================
Connected successfully
Acquired successfully
Released successfully
Querying story: kfeng-EVO-X2/cpu_usage
Found 1234 events in kfeng-EVO-X2/cpu_usage
Disconnected successfully
test_chronolog_service.py::TestChronoLogService::test_query_story_success PASSED

...

============================== 10 passed in 2.34s ==============================
```

**Note**: Tests run in separate subprocesses (due to `--forked`), so execution time may be longer than unit tests.

## Continuous Integration

To integrate with CI/CD pipelines:

```yaml
# Example GitHub Actions
- name: Start ChronoLog Services
  run: |
    # Start ChronoVisor and ChronoKeeper services
    # (implementation depends on your setup)

- name: Run Backend Tests
  run: |
    cd Plugins/chronoviz/grafana_backend
    export CHRONOLOG_LIB_PATH="$HOME/chronolog-install/chronolog/lib"
    export PYTHONPATH="$CHRONOLOG_LIB_PATH:$PYTHONPATH"
    export LD_LIBRARY_PATH="$CHRONOLOG_LIB_PATH:$LD_LIBRARY_PATH"
    pip install -r requirements-test.txt
    pytest test_chronolog_service.py -v --forked

- name: Collect Coverage (Optional)
  run: |
    cd Plugins/chronoviz/grafana_backend
    # WARNING: May cause segfaults without --forked
    coverage run --source=chronolog_service -m pytest test_chronolog_service.py -v
    coverage report -m
    coverage xml
```

## Future Enhancements

Potential test improvements:

1. **Re-enable Commented Tests**: Fix and re-enable `test_connect_disconnect_success`, `test_acquire_release_success`, `test_search_endpoint_not_connected`
2. **Chronicle/Story Listing**: Add tests for `/chronicles` and `/stories` endpoints when implemented
3. **Performance Tests**: Load testing with multiple concurrent requests
4. **Stress Tests**: Large result sets, long-running queries
5. **Security Tests**: Input validation, injection attacks
6. **API Contract Tests**: OpenAPI specification validation
7. **Coverage with Forked Tests**: Investigate ways to collect coverage from forked subprocesses

## Dependencies

Test-specific dependencies (see `requirements-test.txt`):
- `pytest>=7.4.0` - Test framework
- `pytest-forked>=1.6.0` - Run tests in separate subprocesses (prevents C++ client segfaults)
- `pytest-cov>=4.1.0` - Coverage reporting (incompatible with `--forked`)
- `coverage>=7.0.0` - Coverage.py for direct coverage collection
- `httpx>=0.24.0` - HTTP client for FastAPI testing (AsyncClient with ASGITransport)

## Troubleshooting Tests

### Import Errors

If you see import errors for `chronolog_service` or `chronolog`:
```bash
export CHRONOLOG_LIB_PATH="$HOME/chronolog-install/chronolog/lib"
export PYTHONPATH="$CHRONOLOG_LIB_PATH:$PYTHONPATH"
export LD_LIBRARY_PATH="$CHRONOLOG_LIB_PATH:$LD_LIBRARY_PATH"
pytest test_chronolog_service.py --forked
```

### Segmentation Faults

If tests segfault:
- **Ensure `--forked` is used**: Tests must run in separate subprocesses
- **Check ChronoLog services**: Ensure ChronoVisor and ChronoKeeper are running
- **Verify cleanup**: The setup fixture should fully disconnect and clean up the C++ client between tests
- **Check library paths**: Ensure `LD_LIBRARY_PATH` includes the ChronoLog library directory

### Connection Errors

If connection tests fail:
- Verify ChronoLog services are running and accessible
- Check connection parameters (IP, ports, provider IDs)
- Ensure network connectivity to ChronoLog services
- Check service logs for errors

### Coverage "No data collected"

If coverage shows "No data was collected":
- **This is expected with `--forked`**: Coverage cannot be collected from forked subprocesses
- **Solution**: Use `run_tests_with_coverage.sh` which runs without `--forked`
- **Warning**: Tests may segfault without `--forked` due to C++ client state issues

### Fixture Errors

If fixtures fail:
- Check pytest version is >=7.4.0
- Ensure `pytest-forked` is installed
- Verify `httpx` is installed for AsyncClient
- Check that ChronoLog services are accessible

## License

Apache-2.0



