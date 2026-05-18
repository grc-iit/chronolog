#!/bin/bash
# Run integration tests for ChronoLog Grafana Backend
#
# These are integration tests that require ChronoLog services to be running.
# Ensure ChronoVisor and ChronoKeeper are started before running tests.

set -e

CHRONOLOG_INSTALL_PATH="${CHRONOLOG_INSTALL_PATH:-$HOME/chronolog-install}"
if [ ! -d "$CHRONOLOG_INSTALL_PATH/chronolog/lib" ]; then
    echo "WARNING: ChronoLog library not found at $CHRONOLOG_INSTALL_PATH/chronolog/lib"
    echo "Set CHRONOLOG_INSTALL_PATH environment variable to the correct path"
    echo ""
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Running ChronoLog Backend Integration Tests ==="
echo ""
echo "⚠️  IMPORTANT: These tests require ChronoLog services to be running!"
echo "    Make sure ChronoVisor and ChronoKeeper are started."
echo ""

# Check if virtual environment exists
if [ ! -d "venv" ]; then
    echo "Creating virtual environment..."
    python3 -m venv venv
fi

# Activate virtual environment
source venv/bin/activate

# Set library path
export CHRONOLOG_LIB_PATH="$CHRONOLOG_INSTALL_PATH/chronolog/lib"
export PYTHONPATH="$CHRONOLOG_LIB_PATH:$PYTHONPATH"
export LD_LIBRARY_PATH="$CHRONOLOG_LIB_PATH:$LD_LIBRARY_PATH"

# Set default connection parameters (can be overridden by environment)
export CHRONOLOG_PORTAL_IP="${CHRONOLOG_PORTAL_IP:-127.0.0.1}"
export CHRONOLOG_PORTAL_PORT="${CHRONOLOG_PORTAL_PORT:-5555}"
export CHRONOLOG_PORTAL_PROVIDER_ID="${CHRONOLOG_PORTAL_PROVIDER_ID:-55}"
export CHRONOLOG_QUERY_IP="${CHRONOLOG_QUERY_IP:-127.0.0.1}"
export CHRONOLOG_QUERY_PORT="${CHRONOLOG_QUERY_PORT:-5557}"
export CHRONOLOG_QUERY_PROVIDER_ID="${CHRONOLOG_QUERY_PROVIDER_ID:-57}"

echo "Connection Configuration:"
echo "  Portal: ${CHRONOLOG_PORTAL_IP}:${CHRONOLOG_PORTAL_PORT}"
echo "  Query:  ${CHRONOLOG_QUERY_IP}:${CHRONOLOG_QUERY_PORT}"
echo ""

# Install test dependencies
echo "Installing dependencies..."
if ! pip install -q -r requirements-test.txt; then
    echo "ERROR: Failed to install test dependencies"
    echo "Try running: pip install -r requirements-test.txt"
    exit 1
fi

# Verify pytest is available
if ! python -m pytest --version > /dev/null 2>&1; then
    echo "ERROR: pytest is not available after installation"
    echo "Try running: pip install pytest"
    exit 1
fi

# Verify chronolog_service can be imported
echo "Verifying service module can be imported..."
if ! python -c "import chronolog_service" 2>&1; then
    echo "ERROR: Failed to import chronolog_service module"
    echo "Make sure you're in the correct directory: $SCRIPT_DIR"
    echo "And that chronolog_service.py exists"
    exit 1
fi

# Run tests with output visible (-s flag) and logging enabled
# Using --forked to run each test in a separate subprocess to avoid C++ client state issues
echo ""
echo "Running tests (each test in separate subprocess to avoid C++ state issues)..."
if ! pytest test_chronolog_service.py -v -s --setup-show --log-level=DEBUG --forked; then
    echo ""
    echo "⚠️  Tests failed. Check the output above for details."
    echo "    Make sure ChronoLog services are running and accessible."
    exit 1
fi

echo ""
echo "=== Tests Complete ==="
echo ""
echo "NOTE: Coverage collection is disabled when using --forked."
echo "      To collect coverage, use run_tests_with_coverage.sh"
echo "      (but be aware it runs without --forked and may cause segfaults)"

