#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
if [ ! -f build/viz-venv/.prepared ]; then
    bash plugins/chrono-viz/prepare.sh
fi
mkdir -p build/viz build/smoke/wheels
install_node=0
if [ ! -d build/viz/node_modules ] || ! cmp -s plugins/chrono-viz/grafana/package.json build/viz/package.json; then
    install_node=1
fi
cp plugins/chrono-viz/grafana/* build/viz/
cd build/viz
if [ "$install_node" = 1 ]; then
    timeout 480 npm install --no-audit --no-fund
fi
timeout 120 npm run typecheck
timeout 180 npm run build
cp plugin.json logo.svg dist/
cd "$root"
timeout 60 build/viz-venv/bin/python -m build --wheel --no-isolation --outdir build/smoke/wheels plugins/chrono-viz/backend
if [ "$#" -gt 0 ]; then
    mkdir -p build/viz/sdk/chronolog build/viz/sdk/chronolog-4.0.0.dist-info
    cp client/python/chronolog/*.py build/viz/sdk/chronolog/
    cp "$1" build/viz/sdk/chronolog/
    printf 'Metadata-Version: 2.1\nName: chronolog\nVersion: 4.0.0\nRequires-Python: >=3.12\n' > build/viz/sdk/chronolog-4.0.0.dist-info/METADATA
    printf 'Wheel-Version: 1.0\nGenerator: chronolog-viz\nRoot-Is-Purelib: false\nTag: cp312-abi3-linux_x86_64\n' > build/viz/sdk/chronolog-4.0.0.dist-info/WHEEL
    timeout 60 build/viz-venv/bin/python -m wheel pack build/viz/sdk -d build/smoke/wheels
    timeout 60 build/viz-venv/bin/pip install --force-reinstall --no-deps build/smoke/wheels/chronolog-4.0.0-cp312-abi3-linux_x86_64.whl
fi
timeout 60 build/viz-venv/bin/pip install --force-reinstall --no-deps build/smoke/wheels/chronolog_viz-4.0.0-py3-none-any.whl
