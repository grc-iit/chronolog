#!/bin/bash
# Start ChronoLog Grafana Development Environment

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== ChronoLog Grafana Development Environment ==="

# Check if ChronoLog install path is set
CHRONOLOG_INSTALL_PATH="${CHRONOLOG_INSTALL_PATH:-$HOME/chronolog-install}"
if [ ! -d "$CHRONOLOG_INSTALL_PATH/chronolog/lib" ]; then
  echo "WARNING: ChronoLog library not found at $CHRONOLOG_INSTALL_PATH/chronolog/lib"
  echo "Set CHRONOLOG_INSTALL_PATH environment variable to the correct path"
  echo ""
fi

# Build the plugin first
echo "Building Grafana plugin..."
"$SCRIPT_DIR/build_grafana_plugin.sh"

# Check if we should use Docker or run locally
if [ "$1" == "--local" ]; then
  echo ""
  echo "=== Starting Local Development ==="

  # Start Python backend
  echo "Starting ChronoLog backend service..."
  cd "$PROJECT_ROOT/grafana_backend"

  # Create virtual environment if needed
  if [ ! -d "venv" ]; then
    python3 -m venv venv
    source venv/bin/activate
    pip install -r requirements.txt
  else
    source venv/bin/activate
  fi

  # Set library path
  export CHRONOLOG_LIB_PATH="$CHRONOLOG_INSTALL_PATH/chronolog/lib"
  export PYTHONPATH="$CHRONOLOG_LIB_PATH:$PYTHONPATH"
  export LD_LIBRARY_PATH="$CHRONOLOG_LIB_PATH:$LD_LIBRARY_PATH"

  # Start the backend
  python chronolog_service.py &
  BACKEND_PID=$!

  echo "Backend started with PID: $BACKEND_PID"
  echo ""
  echo "=== Services Running ==="
  echo "Backend: http://localhost:8080"
  echo "Backend health: http://localhost:8080/health"
  echo ""
  echo "Now start Grafana and configure the ChronoLog data source"
  echo "Press Ctrl+C to stop the backend"

  # Wait for the backend
  wait $BACKEND_PID
else
  echo ""
  echo "=== Starting Docker Environment ==="

  cd "$PROJECT_ROOT"

  # Export variables for docker-compose
  export CHRONOLOG_INSTALL_PATH

  # Start services
  docker compose up --build
fi
