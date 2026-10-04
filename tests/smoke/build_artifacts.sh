#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
logs=$root/build/smoke
venv=$root/build/smoke-venv
mkdir -p "$logs/wheels" "$logs/tmp"
export TMPDIR="$logs/tmp"
if [ ! -x "$venv/bin/python" ]; then
    python3 -m venv "$venv"
fi
timeout 300 "$venv/bin/pip" install --quiet -r tests/smoke/python/requirements.txt build pytest hatchling 'opentelemetry-sdk>=1.39,<2' 'mcp>=2.2,<3'
timeout 600 cmake --preset dev
timeout 1800 cmake --build --preset dev --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-8}"
# The wheel's pyproject selects the separate python preset and build/python directory.
timeout 600 "$venv/bin/python" -m build --wheel --outdir "$logs/wheels" client/python
timeout 120 "$venv/bin/pip" install --no-deps --force-reinstall "$logs"/wheels/chronolog-4.0.0-*.whl
timeout 360 bash plugins/viz/prepare.sh
timeout 120 build/viz-venv/bin/pip install --no-deps --force-reinstall "$logs"/wheels/chronolog-4.0.0-*.whl
timeout 300 build/viz-venv/bin/pip install --quiet 'opentelemetry-sdk>=1.39,<2' 'mcp>=2.2,<3'
timeout 900 bash plugins/viz/build.sh
timeout 10 build/viz-venv/bin/python -c 'import chronolog, chronolog_viz'
timeout 180 "$venv/bin/python" -m build --wheel --no-isolation --outdir "$logs/wheels" plugins/mcp
timeout 120 "$venv/bin/pip" install --no-deps --force-reinstall "$logs"/wheels/chronolog_mcp-4.0.0-*.whl
timeout 480 bash client/typescript/run.sh
echo 'Smoke artifacts ready'
