#!/bin/bash
# Build the ChronoLog Grafana Plugin

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PLUGIN_DIR="$PROJECT_ROOT/grafana_plugin"

echo "=== Building ChronoLog Grafana Plugin ==="

cd "$PLUGIN_DIR"

# Install dependencies if needed
if [ ! -d "node_modules" ]; then
    echo "Installing dependencies..."
    npm install
fi

# Build the plugin
echo "Building plugin..."
npm run build

echo "=== Build Complete ==="
echo "Plugin built to: $PLUGIN_DIR/dist"
echo ""
echo "To use with Docker, run:"
echo "  cd $PROJECT_ROOT && docker-compose up"
echo ""
echo "To use with standalone Grafana:"
echo "  1. Copy $PLUGIN_DIR/dist to your Grafana plugins directory"
echo "  2. Add 'chronolog-datasource' to GF_PLUGINS_ALLOW_LOADING_UNSIGNED_PLUGINS"
echo "  3. Restart Grafana"

