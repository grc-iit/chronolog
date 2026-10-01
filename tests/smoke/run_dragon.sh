#!/usr/bin/env bash
# Builds the builder and runtime images, brings the compose stack up, runs the
# Python smoke test and tears the stack down, first under rootless Docker and then
# under rootless Podman. Run on dragon from the repository root, normally through
# rbuild:  rbuild 'bash tests/smoke/run_dragon.sh'
# ENGINES="docker" or ENGINES="podman" limits the run to one engine.
set -uo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
logs=$root/build/smoke
mkdir -p "$logs"
compose_file=deploy/compose/compose.yaml
engines=${ENGINES:-"docker podman"}
overall=0

venv=$root/build/smoke-venv
if [ ! -x "$venv/bin/python" ]; then
    python3 -m venv "$venv" || { echo "smoke: cannot create venv"; exit 1; }
fi
timeout 300 "$venv/bin/pip" install --quiet -r tests/smoke/python/requirements.txt \
    || { echo "smoke: pip install failed"; exit 1; }

run_engine() {
    local engine=$1 project=chronolog-smoke-$1 log=$logs/$1.log compose build_cmd
    : > "$log"
    case "$engine" in
        docker)
            export DOCKER_HOST=unix:///run/user/1000/docker.sock
            build_cmd=(docker build)
            compose=(docker compose -p "$project" -f "$compose_file")
            ;;
        podman)
            unset DOCKER_HOST
            systemctl --user start podman.socket >> "$log" 2>&1 || true
            build_cmd=(podman build)
            compose=(podman compose -p "$project" -f "$compose_file")
            ;;
        *) echo "smoke: unknown engine $engine"; return 2 ;;
    esac
    echo "== $engine: $("${build_cmd[0]}" --version)"

    step() {
        local name=$1 limit=$2
        shift 2
        echo "-- $engine: $name"
        timeout "$limit" "$@" >> "$log" 2>&1
        local rc=$?
        if [ "$rc" -ne 0 ]; then
            echo "FAILED $engine: $name (exit $rc), tail of $log:"
            tail -40 "$log"
        fi
        return "$rc"
    }

    local rc=0
    step "build builder image" 5400 "${build_cmd[@]}" -f deploy/containers/builder.Containerfile -t chronolog-builder:local . || return 1
    step "build runtime image" 2400 "${build_cmd[@]}" -f deploy/containers/runtime.Containerfile -t chronolog-runtime:local . || return 1
    if step "compose up --wait" 300 "${compose[@]}" up -d --wait --wait-timeout 120; then
        echo "-- $engine: smoke.py"
        timeout 120 "$venv/bin/python" tests/smoke/python/smoke.py 2>&1 | tee -a "$log"
        rc=${PIPESTATUS[0]}
    else
        rc=1
    fi
    if [ "$rc" -ne 0 ]; then
        echo "-- $engine: container state and visor log"
        timeout 60 "${compose[@]}" ps 2>&1 | tail -20
        timeout 60 "${compose[@]}" logs --no-color --tail 40 chrono-visor 2>&1 | tail -40
    fi
    step "compose down -v" 120 "${compose[@]}" down -v --timeout 20 || rc=1
    return "$rc"
}

for engine in $engines; do
    if run_engine "$engine"; then
        echo "RESULT $engine: PASS"
    else
        echo "RESULT $engine: FAIL"
        overall=1
    fi
done
exit "$overall"
