#!/usr/bin/env bash
# Builds the Visor, Keeper, Player and Grapher natively with the dev preset (the
# vcpkg binary cache makes that fast), wraps them in runtime-local.Containerfile,
# brings the compose stack up, runs the Python smoke test and tears the stack down,
# first under rootless Docker and then under rootless Podman. Nothing is compiled
# inside a container. Run on dragon from the repository root through rbuild:
#   rbuild 'bash tests/smoke/run_dragon.sh'
# ENGINES="docker" or ENGINES="podman" limits the run to one engine.
# SKIP_NATIVE_BUILD=1 reuses the existing build/dev binaries.
set -uo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
logs=$root/build/smoke
mkdir -p "$logs"
compose_file=deploy/compose/compose.yaml
override_file=deploy/compose/smoke.override.yaml
engines=${ENGINES:-"docker podman"}
overall=0

# Binaries to ship.
targets=(chrono_visor chrono_keeper chrono_player chrono_grapher)
stage=$root/build/image-stage
image=chronolog-runtime-local:dev
export CHRONOLOG_IMAGE=$image

if [ -z "${SKIP_NATIVE_BUILD:-}" ]; then
    echo "-- native dev build: ${targets[*]}"
    { cmake --preset dev && cmake --build --preset dev --parallel 12 --target "${targets[@]}"; } > "$logs/native-build.log" 2>&1 \
        || { echo "FAILED native build, tail of $logs/native-build.log:"; tail -40 "$logs/native-build.log"; exit 1; }
fi
rm -rf "$stage"
mkdir -p "$stage"
for target in "${targets[@]}"; do
    binary=$(find "$root/build/dev" -type f -name "$target" -perm -u+x | head -n 1)
    [ -n "$binary" ] || { echo "smoke: $target was not built"; exit 1; }
    cp "$binary" "$stage/"
done
cp deploy/containers/entrypoint.sh "$stage/"

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
            compose=(docker compose -p "$project" -f "$compose_file" -f "$override_file")
            ;;
        podman)
            unset DOCKER_HOST
            systemctl --user start podman.socket >> "$log" 2>&1 || true
            build_cmd=(podman build)
            compose=(podman compose -p "$project" -f "$compose_file" -f "$override_file")
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
    step "build runtime image" 300 "${build_cmd[@]}" -f deploy/containers/runtime-local.Containerfile -t "$image" "$stage" || return 1
    if step "compose up --wait" 300 "${compose[@]}" up -d --wait --wait-timeout 120; then
        echo "-- $engine: smoke.py"
        timeout 240 "$venv/bin/python" tests/smoke/python/smoke.py --engine "$engine" --project "$project" \
            --compose-file "$compose_file" --compose-file "$override_file" 2>&1 | tee -a "$log"
        rc=${PIPESTATUS[0]}
    else
        rc=1
    fi
    timeout 60 "${compose[@]}" logs --no-color > "$logs/$engine-services.log" 2>&1 || true
    if [ "$rc" -ne 0 ]; then
        echo "-- $engine: container state and service logs"
        timeout 60 "${compose[@]}" ps 2>&1 | tail -20
        tail -40 "$logs/$engine-services.log"
        step "capture archive" 30 "${compose[@]}" cp chrono-grapher:/var/lib/chronolog/archive "$logs/$engine-archive" || true
        step "capture WAL" 30 "${compose[@]}" cp chrono-keeper:/var/lib/chronolog/wal "$logs/$engine-wal" || true
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
