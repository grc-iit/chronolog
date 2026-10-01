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
timeout 300 "$venv/bin/pip" install --quiet -r tests/smoke/python/requirements.txt build pytest
timeout 600 cmake --preset dev
timeout 1800 cmake --build --preset dev --parallel 12
# The wheel's pyproject selects the separate python preset and build/python directory.
timeout 600 "$venv/bin/python" -m build --wheel --outdir "$logs/wheels" client/python
timeout 120 "$venv/bin/pip" install --no-deps --force-reinstall "$logs"/wheels/chronolog-4.0.0-*.whl
timeout 480 bash client/typescript/run_dragon.sh
echo 'Smoke artifacts ready'
