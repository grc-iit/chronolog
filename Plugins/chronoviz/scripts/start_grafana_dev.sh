#!/bin/bash
# Start ChronoLog Grafana Development Environment
#
# Default: run Grafana + backend via Docker Compose.
# Use --local to run only the backend locally (Grafana must be started separately).
#
# Usage:
#   ./start_grafana_dev.sh           # Docker (default)
#   ./start_grafana_dev.sh --local   # Local backend only

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Parse optional --conf before the main mode
CHRONOLOG_CONF_FILE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -c|--conf)
      CHRONOLOG_CONF_FILE="$2"
      shift 2
      ;;
    -h|--help)
      echo "Usage: $0 [OPTION] [--local]"
      echo ""
      echo "Start ChronoLog Grafana dev environment. Default is Docker."
      echo ""
      echo "  (no args)   Use Docker Compose (Grafana + backend)"
      echo "  --local     Run backend only on host (venv + env vars)"
      echo "  -c, --conf FILE  Path to ChronoLog client config JSON (default: install_dir/chronolog/conf/default_client_conf.json)"
      echo "  -h, --help  Show this help"
      echo ""
      echo "Environment: GRAFANA_PORT (default 3000), CHRONOLOG_BACKEND_SERVICE_PORT (default 8080)"
      exit 0
      ;;
    *)
      break
      ;;
  esac
done

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

# Export for docker-compose (Docker mode) or use when starting backend (local mode)
export CHRONOLOG_CONF_FILE

# Default: Docker. Use --local for local backend only.
if [ "${1:-}" == "--local" ]; then
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

  # Backend port: from env or default 8080
  CHRONOLOG_BACKEND_SERVICE_PORT="${CHRONOLOG_BACKEND_SERVICE_PORT:-8080}"
  export CHRONOLOG_BACKEND_SERVICE_PORT

  # Start the backend (pass --conf if CHRONOLOG_CONF_FILE is set)
  python chronolog_service.py ${CHRONOLOG_CONF_FILE:+--conf "$CHRONOLOG_CONF_FILE"} &
  BACKEND_PID=$!

  GRAFANA_PORT="${GRAFANA_PORT:-3000}"
  echo "Backend started with PID: $BACKEND_PID"
  echo ""
  echo "=== Services Running ==="
  echo "Backend: http://localhost:${CHRONOLOG_BACKEND_SERVICE_PORT}"
  echo "Backend health: http://localhost:${CHRONOLOG_BACKEND_SERVICE_PORT}/health"
  echo "Grafana (start separately): http://localhost:${GRAFANA_PORT}"
  echo ""
  echo "Now start Grafana and configure the ChronoLog data source"
  echo "Press Ctrl+C to stop the backend"

  # Wait for the backend
  wait $BACKEND_PID
else
  echo ""
  echo "=== Starting Docker Environment ==="

  cd "$PROJECT_ROOT"

  # Use docker compose (V2) if available, else docker-compose (V1)
  if docker compose version >/dev/null 2>&1; then
    DOCKER_COMPOSE="docker compose"
  elif command -v docker-compose >/dev/null 2>&1; then
    DOCKER_COMPOSE="docker-compose"
  else
    echo "ERROR: Neither 'docker compose' nor 'docker-compose' found. Install Docker Compose."
    exit 1
  fi
  echo "Using: $DOCKER_COMPOSE"
  GRAFANA_PORT="${GRAFANA_PORT:-3000}"
  echo "Grafana: http://localhost:${GRAFANA_PORT}"
  echo "Backend: http://localhost:${CHRONOLOG_BACKEND_SERVICE_PORT:-8080}"
  echo ""

  # Export variables for docker-compose
  export CHRONOLOG_INSTALL_PATH
  export GRAFANA_PORT

  # Start services
  $DOCKER_COMPOSE up --build
fi
