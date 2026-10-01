#!/usr/bin/env bash
set -eu
visor=$1 keeper=$2 player=$3 python=$4 tests=$5
scratch=$(mktemp -d)
pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill -KILL "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    rm -rf "$scratch"
}
trap cleanup EXIT
port=$((10000 + (RANDOM % 4400) * 5))
cat > "$scratch/visor.json" <<JSON
{"listen":"127.0.0.1:$port","internal_listen":"127.0.0.1:$((port+1))","db_path":"$scratch/catalog.sqlite","keepers":[{"process_id":"keeper-1","endpoint":"127.0.0.1:$((port+2))"}],"player":"127.0.0.1:$((port+4))"}
JSON
cat > "$scratch/keeper.json" <<JSON
{"listen":"127.0.0.1:$((port+2))","internal_listen":"127.0.0.1:$((port+3))","self_endpoint":"127.0.0.1:$((port+2))","visor_internal":"127.0.0.1:$((port+1))","wal_dir":"$scratch/wal","worker_threads":4,"heartbeat_interval_ms":100,"story_chunk_duration_secs":60,"shutdown_confirm_timeout_secs":1}
JSON
cat > "$scratch/player.json" <<JSON
{"listen":"127.0.0.1:$((port+4))","advertise":"127.0.0.1:$((port+4))","visor":"127.0.0.1:$port","visor_internal":"127.0.0.1:$((port+1))","keeper_internal":{"keeper-1":"127.0.0.1:$((port+3))"},"tail_poll_ms":10,"batch_size":128}
JSON
start() {
    local binary=$1 role=$2 ready=$3
    "$binary" --config "$scratch/$role.json" > "$scratch/$role.log" 2>&1 &
    pids+=("$!")
    for _ in $(seq 1 100); do
        if rg -q "$ready" "$scratch/$role.log"; then return; fi
        kill -0 "${pids[-1]}" 2>/dev/null || { cat "$scratch/$role.log"; return 1; }
        sleep 0.1
    done
    cat "$scratch/$role.log"
    return 1
}
start "$visor" visor 'catalog ready'
start "$keeper" keeper 'journal ready'
start "$player" player 'player ready'
if ! timeout 60 env CHRONOLOG_TEST_VISOR="127.0.0.1:$port" CHRONOLOG_TEST_PLAYER="127.0.0.1:$((port+4))" "$python" -m pytest -q "$tests"; then
    cat "$scratch/visor.log" "$scratch/keeper.log" "$scratch/player.log"
    exit 1
fi
