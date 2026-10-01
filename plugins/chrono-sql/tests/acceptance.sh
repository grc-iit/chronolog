#!/usr/bin/env bash
set -eu
visor=$1 keeper=$2 player=$3 client=$4
scratch=$(mktemp -d)
pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill -KILL "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    rm -rf "$scratch"
}
trap cleanup EXIT
port=$((20000 + RANDOM % 20000))
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
timeout 60 "$client" "127.0.0.1:$port" "127.0.0.1:$((port+4))" "$scratch" &
client_pid=$!
pids+=("$client_pid")
for _ in $(seq 1 1200); do
    if [ -f "$scratch/stop-keeper" ] && [ ! -f "$scratch/keeper-stopped" ]; then
        kill -KILL "${pids[1]}"
        wait "${pids[1]}" 2>/dev/null || true
        touch "$scratch/keeper-stopped"
    fi
    if ! kill -0 "$client_pid" 2>/dev/null; then break; fi
    sleep 0.05
done
if ! wait "$client_pid"; then
    cat "$scratch/visor.log" "$scratch/keeper.log" "$scratch/player.log"
    exit 1
fi
