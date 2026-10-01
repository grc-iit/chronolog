#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
python3.12 -m venv build/viz-venv
timeout 300 build/viz-venv/bin/pip install 'nanobind>=2.4' 'wheel>=0.45' 'build>=1.2' 'hatchling>=1.27' 'pytest>=8,<9' 'httpx>=0.28,<1' 'requests>=2.32,<3' 'fastapi>=0.115,<1' 'uvicorn>=0.34,<1'
touch build/viz-venv/.prepared
