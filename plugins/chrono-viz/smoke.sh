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
    timeout 60 "${compose[@]}" down -v --timeout 10 || true
}
trap cleanup EXIT
timeout 240 "${compose[@]}" build chrono-viz
timeout 240 "${compose[@]}" up -d --wait --wait-timeout 180
timeout 60 build/viz-venv/bin/python plugins/chrono-viz/backend/smoke.py
