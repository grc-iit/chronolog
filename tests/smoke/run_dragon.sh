#!/usr/bin/env bash
# Builds the Visor, Keeper, Player and Grapher natively with the dev preset (the
# vcpkg binary cache makes that fast), wraps them in runtime-local.Containerfile,
# brings the compose stack up, runs the Python smoke test and tears the stack down,
# first under rootless Docker and then under rootless Podman. Nothing is compiled
# inside a container. Run on dragon from the repository root through rbuild:
#   RBUILD_LOCK=stack rbuild 'bash tests/smoke/run_dragon.sh'
# ENGINES="docker" or ENGINES="podman" limits the run to one engine.
# SKIP_NATIVE_BUILD=1 reuses build/dev; SKIP_BINDING_BUILD=1 requires build_artifacts.sh outputs.
# CHRONOLOG_IMAGE_TAG (rbuild sets wt-<worktree>) tags the runtime, viz and MCP images so two trees on one
# host never replace each other's image.
set -uo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
logs=$root/build/smoke
mkdir -p "$logs/tmp"
export TMPDIR="$logs/tmp"
compose_file=deploy/compose/compose.yaml
override_file=deploy/compose/smoke.override.yaml
python_override=$logs/python.override.yaml
cp deploy/compose/demo.override.yaml "$python_override"
engines=${ENGINES:-"docker podman"}
overall=0

# Binaries to ship, shared with deploy/demo/chronolog-demo.
mapfile -t targets < <(bash deploy/demo/stage.sh targets)
stage=$root/build/image-stage
image=chronolog-runtime-local:${CHRONOLOG_IMAGE_TAG:-dev}
export CHRONOLOG_IMAGE=$image
export CHRONOLOG_VIZ_IMAGE=chronolog-viz:${CHRONOLOG_IMAGE_TAG:-4.0.0}
export CHRONOLOG_MCP_IMAGE=chronolog-mcp:${CHRONOLOG_IMAGE_TAG:-4.0.0}

if [ -z "${SKIP_NATIVE_BUILD:-}" ]; then
    echo "-- native dev build: ${targets[*]}"
    { bash plugins/chrono-viz/prepare.sh && cmake --preset dev -DCHRONOLOG_BUILD_PYTHON=ON -DPython_EXECUTABLE="$root/build/viz-venv/bin/python" && cmake --build --preset dev --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-8}" --target "${targets[@]}" chronolog_viz; } > "$logs/native-build.log" 2>&1 \
        || { echo "FAILED native build, tail of $logs/native-build.log:"; tail -40 "$logs/native-build.log"; exit 1; }
fi
bash deploy/demo/stage.sh binaries "$root" || exit 1

venv=$root/build/smoke-venv
if [ "${SKIP_BINDING_BUILD:-}" = 1 ]; then
    [ -x "$venv/bin/python" ] || { echo "smoke: missing smoke venv; run tests/smoke/build_artifacts.sh"; exit 1; }
    wheels=("$logs"/wheels/chronolog-4.0.0-*.whl)
    [ -f "${wheels[0]}" ] || { echo "smoke: missing Python wheel; run tests/smoke/build_artifacts.sh"; exit 1; }
    package=$root/build/typescript/package
    [ -f "$package/build/dev/chronolog_node.node" ] && [ -f "$package/dist/index.js" ] \
        || { echo "smoke: missing TypeScript package; run tests/smoke/build_artifacts.sh"; exit 1; }
    [ -f "$logs/wheels/chronolog_mcp-4.0.0-py3-none-any.whl" ] || { echo "smoke: missing MCP wheel; run tests/smoke/build_artifacts.sh"; exit 1; }
    [ -f "$root/build/viz/dist/module.js" ] && [ -f "$logs/wheels/chronolog_viz-4.0.0-py3-none-any.whl" ] \
        || { echo "smoke: missing viz artifacts; run tests/smoke/build_artifacts.sh"; exit 1; }
    timeout 10 "$root/build/viz-venv/bin/python" -c 'import chronolog, chronolog_viz' \
        || { echo "smoke: missing viz dependencies; run tests/smoke/build_artifacts.sh"; exit 1; }
    timeout 10 "$venv/bin/python" -c 'import grpc, grpc_tools.protoc, pytest, mcp, opentelemetry.sdk' \
        || { echo "smoke: missing test dependencies; run tests/smoke/build_artifacts.sh"; exit 1; }
else
    if [ ! -x "$venv/bin/python" ]; then
        python3 -m venv "$venv" || { echo "smoke: cannot create venv"; exit 1; }
    fi
    timeout 300 "$venv/bin/pip" install --quiet -r tests/smoke/python/requirements.txt \
        || { echo "smoke: pip install failed"; exit 1; }
fi

if [ "${RBUILD_HELD:-}" != stack ] && [ "${CHRONOLOG_STACK_LOCKED:-0}" != 1 ]; then
    exec flock "$HOME/chronolog-sprint/stack.lock" env CHRONOLOG_STACK_LOCKED=1 SKIP_NATIVE_BUILD=1 bash "$0" "$@"
fi

run_engine() {
    local engine=$1 project=chronolog-smoke-$1 log=$logs/$1.log compose build_cmd
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
    step "build runtime image" 300 bash deploy/demo/stage.sh image "${build_cmd[0]}" "$root" "$image" || return 1
    if step "compose up --wait" 300 "${compose[@]}" up -d --wait --wait-timeout 120; then
        stack_ready=1
        if [ "${CHRONOLOG_SMOKE_STREAM_ONLY:-0}" = 1 ]; then
            for ((stream_run=1; stream_run<=${CHRONOLOG_SMOKE_STREAM_RUNS:-1}; ++stream_run)); do
                step "chrono-stream pass $stream_run" 360 bash plugins/chrono-stream/tests/smoke.sh "$engine" "$project" || { rc=1; break; }
                cp "$logs/$engine-stream-services.log" "$logs/$engine-stream-$stream_run-services.log"
            done
            step "compose down -v" 120 "${compose[@]}" down -v --timeout 20 || rc=1
            return "$rc"
        fi
        if [ "${CHRONOLOG_SMOKE_VIZ_ONLY:-0}" != 1 ]; then
            step "chrono-kvs put get get-at history" 45 ./build/dev/plugins/chrono-kvs/chronolog_kvs_example \
                127.0.0.1:50051 127.0.0.1:50054 || rc=1
            step "chrono-pubsub publish subscribe saved KVS position" 45 ./build/dev/plugins/chrono-pubsub/chronolog_pubsub_example \
                127.0.0.1:50051 127.0.0.1:50054 || rc=1
            step "chrono-sql typed provenance SQL reads" 45 ./build/dev/plugins/chrono-sql/chronolog_sql_example \
                127.0.0.1:50051 127.0.0.1:50054 || rc=1
            ldms_container="ldms-smoke-$engine-$(date +%s)"
            step "chrono-ldms fake ldmsd stores samples" 60 ./build/dev/plugins/chrono-ldms/chronolog_ldms_fake_ldmsd \
                --catalog 127.0.0.1:50051 --player 127.0.0.1:50054 --container "$ldms_container" --producers 3 --samples 20 --stale-every 5 || rc=1
            step "chrono-ldms read samples back" 60 ./build/dev/plugins/chrono-ldms/chronolog_ldms_example \
                127.0.0.1:50051 127.0.0.1:50054 "$ldms_container" 60 || rc=1
            step "chrono-stream collect export InfluxDB query Grafana health" 360 bash plugins/chrono-stream/tests/smoke.sh "$engine" "$project" || rc=1
        fi
        step "chrono-viz Replay backend Grafana proxy health and plugin" 480 bash plugins/chrono-viz/smoke.sh "$engine" "$project" || rc=1
        if [ "${CHRONOLOG_SMOKE_PLUGINS_ONLY:-0}" = 1 ]; then
            step "compose down -v" 120 "${compose[@]}" down -v --timeout 20 || rc=1
            return "$rc"
        fi
        echo "-- $engine: smoke.py"
        local smoke_files=() index
        for ((index=5; index<${#compose[@]}; index+=2)); do
            smoke_files+=(--compose-file "${compose[index]}")
        done
        timeout 240 "$venv/bin/python" tests/smoke/python/smoke.py --engine "$engine" --project "$project" \
            "${smoke_files[@]}" 2>&1 | tee -a "$log"
        local python_rc=${PIPESTATUS[0]}
        if [ "$python_rc" -ne 0 ]; then rc=$python_rc; fi
        if [ "$rc" -eq 0 ]; then
            if [ "${SKIP_BINDING_BUILD:-}" != 1 ]; then
                step "Python wheel build dependencies" 180 "$venv/bin/pip" install --quiet build pytest hatchling || rc=1
                step "Python abi3 wheel" 600 "$venv/bin/python" -m build --wheel --outdir "$logs/wheels" client/python || rc=1
            fi
            if [ "$rc" -eq 0 ]; then
                step "install Python wheel" 120 "$venv/bin/pip" install --no-deps --force-reinstall "$logs"/wheels/chronolog-4.0.0-*.whl || rc=1
                if [ "${SKIP_BINDING_BUILD:-}" != 1 ]; then
                    step "Python telemetry dependencies" 180 "$venv/bin/pip" install 'opentelemetry-sdk>=1.39,<2' || rc=1
                fi
                step "Python SDK pytest" 120 env CHRONOLOG_TEST_VISOR=127.0.0.1:50051 CHRONOLOG_TEST_PLAYER=127.0.0.1:50054 \
                    "$venv/bin/python" -m pytest -q client/python/tests || rc=1
                if [ -f deploy/demo/tour.py ]; then
                    step "chronolog-demo tour against the smoke stack" 420 bash deploy/demo/chronolog-demo tour \
                        --engine "$engine" --project "$project" || rc=1
                else
                    echo "-- $engine: deploy/demo/tour.py is absent, tour step skipped"
                fi
                if [ "$rc" -eq 0 ]; then
                    if [ "${SKIP_BINDING_BUILD:-}" != 1 ]; then
                        step "MCP plugin wheel" 180 "$venv/bin/python" -m build --wheel --no-isolation --outdir "$logs/wheels" plugins/chrono-mcp || rc=1
                        step "install MCP plugin" 180 "$venv/bin/pip" install --force-reinstall --no-deps "$logs"/wheels/chronolog_mcp-4.0.0-*.whl || rc=1
                        step "MCP plugin dependencies" 180 "$venv/bin/pip" install 'mcp>=1.30,<2' || rc=1
                    fi
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
    else
        rc=1
    fi
    if [ "$stack_ready" -eq 1 ]; then
        timeout 60 "${compose[@]}" logs --no-color > "$logs/$engine-host-services.log" 2>&1 || true
        if [ "$rc" -ne 0 ]; then cat "$logs/$engine-host-services.log"; fi
        # Acquired endpoints must be reachable from the SDK client's network.
        if ! step "reset host SDK stack" 120 "${compose[@]}" down -v --timeout 20; then
            return 1
        fi
        compose=("${compose[@]:0:${#compose[@]}-2}")
        if ! step "compose network SDK stack" 300 "${compose[@]}" up -d --wait --wait-timeout 120; then
            step "compose down -v" 120 "${compose[@]}" down -v --timeout 20 || true
            return 1
        fi
        if [ "${SKIP_BINDING_BUILD:-}" = 1 ] || step "build TypeScript binding" 480 bash client/typescript/run_dragon.sh; then
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

if [ "${RBUILD_HELD:-}" != stack ]; then
    exec 8>"$HOME/chronolog-sprint/stack.lock"
    echo "-- waiting for shared stack lock"
    flock 8
    export RBUILD_HELD=stack
fi

for engine in $engines; do
    if run_engine "$engine"; then
        echo "RESULT $engine: PASS"
    else
        echo "RESULT $engine: FAIL"
        overall=1
    fi
done
exit "$overall"
