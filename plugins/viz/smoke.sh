#!/usr/bin/env bash
set -euo pipefail
engine=$1 project=$2
if [ "${RBUILD_HELD:-}" != stack ] && [ "${CHRONOLOG_STACK_LOCKED:-0}" != 1 ]; then
    exec flock "$HOME/chronolog-sprint/stack.lock" env CHRONOLOG_STACK_LOCKED=1 bash "$0" "$@"
fi
case "$engine" in
    docker) export DOCKER_HOST=unix:///run/user/1000/docker.sock; compose=(docker compose) ;;
    podman) unset DOCKER_HOST; compose=(podman compose) ;;
    *) exit 2 ;;
esac
compose+=(-p "$project" -f deploy/compose/compose.yaml -f deploy/compose/smoke.override.yaml -f build/smoke/python.override.yaml -f deploy/compose/viz.override.yaml)
cleanup() {
    timeout 30 "${compose[@]}" logs --no-color chrono-viz grafana > "build/smoke/$engine-viz-services.log" 2>&1 || true
    timeout 60 "${compose[@]}" rm -s -f chrono-viz grafana || true
}
trap cleanup EXIT
timeout 240 "${compose[@]}" build chrono-viz
if ! timeout 240 "${compose[@]}" up -d --no-deps --no-recreate --wait --wait-timeout 180 chrono-viz grafana; then
    container=$(timeout 10 "${compose[@]}" ps -q chrono-viz)
    timeout 10 "$engine" inspect --format '{{json .State}}' "$container" || true
    timeout 10 build/viz-venv/bin/python - <<'PYTHON' || true
import requests
response = requests.get("http://127.0.0.1:8087/health", timeout=5)
print("Backend health:", response.status_code, response.text, flush=True)
PYTHON
    exit 1
fi
timeout 60 build/viz-venv/bin/python plugins/viz/backend/smoke.py
cleanup
trap - EXIT
timeout 10 build/viz-venv/bin/python - <<'PY'
from chronolog_viz import health
assert health()["status"] == "healthy"
print("PASS base Visor and Player survive viz cleanup", flush=True)
PY
