#!/usr/bin/env bash
# S14.3: internal listeners refuse to start on a wildcard address unless insecure_bind_all is set,
# start when it is set, and start on a loopback cluster interface without it.
# Usage: bind_guard_test.sh VISOR KEEPER GRAPHER
set -u
visor=$1 keeper=$2 grapher=$3
scratch=$(mktemp -d)
pids=()
stop() {
    for pid in "${pids[@]}"; do kill -KILL "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    pids=()
}
trap 'stop; rm -rf "$scratch"' EXIT

fail() { echo "FAIL: $*"; exit 1; }

write_configs() {
    local visor_internal=$1 keeper_internal=$2 grapher_internal=$3
    cat > "$scratch/visor.json" <<JSON
{"listen":"127.0.0.1:$port","internal_listen":"$visor_internal","db_path":"$scratch/catalog.sqlite","keepers":[{"process_id":"keeper-1","endpoint":"127.0.0.1:$((port+2))"}],"player":"127.0.0.1:$((port+4))"}
JSON
    cat > "$scratch/keeper.json" <<JSON
{"listen":"127.0.0.1:$((port+2))","internal_listen":"$keeper_internal","self_endpoint":"127.0.0.1:$((port+2))","visor_internal":"127.0.0.1:$((port+1))","wal_dir":"$scratch/wal","worker_threads":4,"heartbeat_interval_ms":100,"story_chunk_duration_secs":60,"shutdown_confirm_timeout_secs":1}
JSON
    cat > "$scratch/grapher.json" <<JSON
{"process_id":"grapher-1","internal_listen":"$grapher_internal","self_endpoint":"127.0.0.1:$((port+4))","visor_internal":"127.0.0.1:$((port+1))","archive_root":"$scratch/archive"}
JSON
}

# refuses ROLE BINARY [ARGS...]: the process must exit non-zero on its own with a message naming the bind guard.
refuses() {
    local role=$1 binary=$2
    shift 2
    timeout 20 "$binary" --config "$scratch/$role.json" "$@" > "$scratch/$role.refuse.log" 2>&1
    local rc=$?
    [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] || fail "$role did not refuse a wildcard bind (exit $rc)"
    grep -q "insecure.bind.all" "$scratch/$role.refuse.log" || fail "$role refused without naming insecure_bind_all: $(cat "$scratch/$role.refuse.log")"
}

# starts ROLE READY_PATTERN BINARY [ARGS...]: the process must stay alive and print its ready line.
starts() {
    local role=$1 ready=$2 binary=$3
    shift 3
    "$binary" --config "$scratch/$role.json" "$@" > "$scratch/$role.log" 2>&1 &
    pids+=("$!")
    for _ in $(seq 1 300); do
        if grep -q "$ready" "$scratch/$role.log"; then return 0; fi
        kill -0 "${pids[-1]}" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

run_checks() {
    rm -rf "$scratch/wal" "$scratch/archive" "$scratch"/catalog.sqlite*
    mkdir -p "$scratch/archive"
    stop

    write_configs "0.0.0.0:$((port+1))" "127.0.0.1:$((port+3))" "127.0.0.1:$((port+4))"
    refuses visor "$visor"
    CHRONOLOG_VISOR_INSECURE_BIND_ALL=true starts visor 'catalog ready' "$visor" || return 1
    stop

    write_configs "127.0.0.1:$((port+1))" "0.0.0.0:$((port+3))" "0.0.0.0:$((port+4))"
    starts visor 'catalog ready' "$visor" || return 1
    refuses keeper "$keeper"
    CHRONOLOG_KEEPER_INSECURE_BIND_ALL=true starts keeper 'journal ready' "$keeper" || return 1
    kill -KILL "${pids[-1]}"; wait "${pids[-1]}" 2>/dev/null; unset 'pids[-1]'
    refuses grapher "$grapher"
    starts grapher 'grapher ready' "$grapher" --insecure-bind-all || return 1
    kill -KILL "${pids[-1]}"; wait "${pids[-1]}" 2>/dev/null; unset 'pids[-1]'

    write_configs "127.0.0.1:$((port+1))" "127.0.0.1:$((port+3))" "127.0.0.1:$((port+4))"
    starts keeper 'journal ready' "$keeper" || return 1
    starts grapher 'grapher ready' "$grapher" || return 1
    return 0
}

# Ports stay below the kernel ephemeral range; a start that loses a bind race retries on fresh ports.
for attempt in 1 2 3; do
    port=$((10000 + (RANDOM % 4400) * 5))
    if run_checks; then
        echo "bind guard: visor, keeper and grapher refuse wildcard without insecure_bind_all, start with it, and start on loopback"
        exit 0
    fi
    cat "$scratch"/*.log 2>/dev/null
    stop
done
fail "a service did not start with a permitted bind after 3 attempts"
