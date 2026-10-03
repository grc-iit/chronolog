#!/usr/bin/env bash
# Static real-process lease lifecycle gate (I3.6, I3.8, W10.12, W10.18).
set -eu
visor=$1 keeper=$2 grapher=$3 player=$4 driver=$5
scratch=$(mktemp -d)
declare -A pid_of
stop() {
    for pid in "${pid_of[@]}"; do kill -KILL "$pid" 2>/dev/null || true; done
    for pid in "${pid_of[@]}"; do wait "$pid" 2>/dev/null || true; done
    pid_of=()
}
trap 'stop; rm -rf "$scratch"' EXIT
start() {
    local role=$1 log=$2 binary=$3 ready=$4
    "$binary" --config "$scratch/$role.json" > "$scratch/$log.log" 2>&1 &
    pid_of[$role]=$!
    for _ in $(seq 1 300); do
        if grep -q "$ready" "$scratch/$log.log"; then return; fi
        kill -0 "${pid_of[$role]}" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
configure() {
    rm -rf "$scratch/wal" "$scratch/archive" "$scratch"/catalog.sqlite* "$scratch/out"
    mkdir -p "$scratch/out" "$scratch/archive"
    cat > "$scratch/visor.json" <<JSON
{"listen":"127.0.0.1:$port","internal_listen":"127.0.0.1:$((port+1))","db_path":"$scratch/catalog.sqlite","keepers":[{"process_id":"keeper-1","endpoint":"127.0.0.1:$((port+2))"}],"graphers":["127.0.0.1:$gport"],"player":"127.0.0.1:$((port+4))","heartbeat_timeout_ms":1000,"release_fence_timeout_ms":500,"acquisition_lease_default_ns":3000000000,"acquisition_lease_min_ns":3000000000,"lease_safety_margin_ms":500,"acquisition_service_gap_ms":400}
JSON
    cat > "$scratch/keeper.json" <<JSON
{"process_id":"keeper-1","listen":"127.0.0.1:$((port+2))","internal_listen":"127.0.0.1:$((port+3))","self_endpoint":"127.0.0.1:$((port+2))","visor_internal":"127.0.0.1:$((port+1))","wal_dir":"$scratch/wal","worker_threads":4,"heartbeat_interval_ms":100,"story_chunk_duration_secs":1,"seal_interval_ms":100,"archive_visibility_delay_secs":1,"watermark_resend_timeout_secs":1,"shutdown_confirm_timeout_secs":1}
JSON
    cat > "$scratch/grapher.json" <<JSON
{"process_id":"grapher-1","internal_listen":"127.0.0.1:$gport","self_endpoint":"127.0.0.1:$gport","visor_internal":"127.0.0.1:$((port+1))","archive_root":"$scratch/archive","heartbeat_interval_ms":200}
JSON
    cat > "$scratch/player.json" <<JSON
{"listen":"127.0.0.1:$((port+4))","advertise":"127.0.0.1:$((port+4))","visor":"127.0.0.1:$port","visor_internal":"127.0.0.1:$((port+1))","keeper_internal":{"keeper-1":"127.0.0.1:$((port+3))"},"archive_root":"$scratch/archive","manifest_poll_ms":100,"tail_poll_ms":10}
JSON
}
launch() {
    configure
    start visor visor "$visor" 'catalog ready' && start keeper keeper "$keeper" 'journal ready' &&
        start grapher grapher "$grapher" 'grapher ready' && start player player "$player" 'player ready'
}
dump() { tail -n 40 "$scratch"/*.log 2>/dev/null || true; }
# Ports stay below the kernel ephemeral range so outgoing connections of parallel tests cannot hold them;
# a launch that loses a bind race retries on fresh ports. The Grapher takes its own block.
for attempt in 1 2 3; do
    port=$((10000 + (RANDOM % 4400) * 5))
    gport=$((10000 + (RANDOM % 4400) * 5))
    [ "$gport" -ne "$port" ] || continue
    if launch; then break; fi
    dump
    stop
    [ "$attempt" -lt 3 ] || exit 1
done
if ! timeout 60 "$driver" "127.0.0.1:$port"; then
    echo "FAIL lease lifecycle"
    dump
    exit 1
fi
echo "LEASE LIFECYCLE PASSED"
