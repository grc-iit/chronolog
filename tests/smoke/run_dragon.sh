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
python_override=$logs/python.override.yaml
kvs_catalog_port=50051
kvs_keeper_port=50052
kvs_player_port=50054
if [ "${KVS_ONLY:-0}" = 1 ]; then
    kvs_catalog_port=35051
    kvs_keeper_port=35052
    kvs_player_port=35054
    sed -e "s/127.0.0.1:50051:50051/127.0.0.1:$kvs_catalog_port:50051/" \
        -e "s/127.0.0.1:50052:50052/127.0.0.1:$kvs_keeper_port:50052/" \
        -e "s/127.0.0.1:50054:50054/127.0.0.1:$kvs_player_port:50054/" \
        "$compose_file" > "$logs/kvs.compose.yaml"
    compose_file=$logs/kvs.compose.yaml
fi
cat > "$python_override" <<YAML
services:
  chrono-visor:
    environment:
      CHRONOLOG_VISOR_KEEPERS: "keeper-1=127.0.0.1:$kvs_keeper_port"
      CHRONOLOG_VISOR_PLAYER: "127.0.0.1:$kvs_player_port"
  chrono-keeper:
    environment:
      CHRONOLOG_KEEPER_SELF_ENDPOINT: "127.0.0.1:$kvs_keeper_port"
YAML
engines=${ENGINES:-"docker podman"}
overall=0

# Binaries to ship.
targets=(chrono_visor chrono_keeper chrono_player chrono_grapher chronolog_kvs_example)
stage=$root/build/image-stage
image=chronolog-runtime-local:dev
if [ "${KVS_ONLY:-0}" = 1 ]; then image=chronolog-kvs-$(basename "$root"):dev; fi
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
if [ "${KVS_ONLY:-0}" != 1 ]; then
    timeout 300 "$venv/bin/pip" install --quiet -r tests/smoke/python/requirements.txt \
        || { echo "smoke: pip install failed"; exit 1; }
fi

run_engine() {
    local engine=$1 project=chronolog-smoke-$1 log=$logs/$1.log compose build_cmd
    if [ "${KVS_ONLY:-0}" = 1 ]; then project=chronolog-kvs-$engine-$(basename "$root"); fi
    : > "$log"
    case "$engine" in
        docker)
            export DOCKER_HOST=unix:///run/user/1000/docker.sock
            build_cmd=(docker build)
            compose=(docker compose -p "$project" -f "$compose_file" -f "$override_file" -f "$python_override")
            ;;
        podman)
            unset DOCKER_HOST
            systemctl --user start podman.socket >> "$log" 2>&1 || true
            build_cmd=(podman build)
            compose=(podman compose -p "$project" -f "$compose_file" -f "$override_file" -f "$python_override")
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

    local rc=0 stack_ready=0
    step "build runtime image" 300 "${build_cmd[@]}" -f deploy/containers/runtime-local.Containerfile -t "$image" "$stage" || return 1
    if step "compose up --wait" 300 "${compose[@]}" up -d --wait --wait-timeout 120; then
        stack_ready=1
        step "chrono-kvs put get get-at history" 45 ./build/dev/plugins/chrono-kvs/modern/chronolog_kvs_example \
            "127.0.0.1:$kvs_catalog_port" "127.0.0.1:$kvs_player_port" || rc=1
        if [ "${KVS_ONLY:-0}" != 1 ]; then
            echo "-- $engine: smoke.py"
            timeout 240 "$venv/bin/python" tests/smoke/python/smoke.py --engine "$engine" --project "$project" \
                --compose-file "$compose_file" --compose-file "$override_file" 2>&1 | tee -a "$log"
            local python_rc=${PIPESTATUS[0]}
            if [ "$python_rc" -ne 0 ]; then rc=$python_rc; fi
            if [ "$rc" -eq 0 ]; then
                step "Python wheel build dependencies" 180 "$venv/bin/pip" install --quiet build pytest hatchling || rc=1
                step "Python abi3 wheel" 600 "$venv/bin/python" -m build --wheel --outdir "$logs/wheels" client/python || rc=1
                if [ "$rc" -eq 0 ]; then
                    step "install Python wheel" 120 "$venv/bin/pip" install --force-reinstall "$logs"/wheels/chronolog-4.0.0-*.whl 'opentelemetry-sdk>=1.39,<2' || rc=1
                    step "Python SDK pytest" 120 env CHRONOLOG_TEST_VISOR=127.0.0.1:50051 CHRONOLOG_TEST_PLAYER=127.0.0.1:50054 \
                        "$venv/bin/python" -m pytest -q client/python/tests || rc=1
                    if [ "$rc" -eq 0 ]; then
                        step "MCP plugin wheel" 180 "$venv/bin/python" -m build --wheel --no-isolation --outdir "$logs/wheels" plugins/chrono-mcp || rc=1
                        step "install MCP plugin" 180 "$venv/bin/pip" install --force-reinstall --no-deps "$logs"/wheels/chronolog_mcp-4.0.0-*.whl || rc=1
                        step "MCP plugin dependencies" 180 "$venv/bin/pip" install 'mcp>=1.30,<2' || rc=1
                        step "MCP plugin pytest" 120 env CHRONOLOG_TEST_VISOR=127.0.0.1:50051 CHRONOLOG_TEST_PLAYER=127.0.0.1:50054 \
                            "$venv/bin/python" -m pytest -q plugins/chrono-mcp/tests || rc=1
                        if [ "$rc" -eq 0 ]; then
                            step "MCP plugin deployment image" 300 "${compose[@]}" -f deploy/compose/mcp.override.yaml build chrono-mcp || rc=1
                            step "MCP plugin container entrypoint" 30 "${compose[@]}" -f deploy/compose/mcp.override.yaml \
                                run --rm --no-deps chrono-mcp --help || rc=1
                        fi
                    fi
                fi
            fi
        fi
    else
        rc=1
    fi
    if [ "$stack_ready" -eq 1 ] && [ "${KVS_ONLY:-0}" != 1 ]; then
        if step "build TypeScript binding" 480 bash client/typescript/run_dragon.sh; then
            export CHRONOLOG_TYPESCRIPT_PACKAGE="$root/build/typescript/package"
            step "TypeScript binding suite" 180 "${compose[@]}" -f client/typescript/test/compose.yaml \
                run --rm --no-deps typescript-tests || rc=1
        else
            rc=1
        fi
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
