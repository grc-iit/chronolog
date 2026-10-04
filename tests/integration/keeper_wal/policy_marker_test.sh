#!/usr/bin/env bash
# Policy marker gate (I8.12, I6.10) on a real stack. A Keeper whose WAL lacks the policy record registers without the
# policy, so the story's physical_policy flag is false and its archive chunk carries no marker: a physical Read of the
# archived event returns it and is never complete (PHYSICAL_AXIS_UNBOUNDED) while the HLC Read is complete. The same
# stack with a fresh WAL is the control: the physical Read becomes complete.
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
    local role=$1 binary=$2 ready=$3
    "$binary" --config "$scratch/$role.json" > "$scratch/$role.log" 2>&1 &
    pid_of[$role]=$!
    for _ in $(seq 1 300); do
        if grep -q "$ready" "$scratch/$role.log"; then return; fi
        kill -0 "${pid_of[$role]}" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
# One WAL segment holding only the record Q0: length, CRC32C and payload, as wal::frame writes them.
seed_wal_without_policy() {
    mkdir -p "$scratch/wal"
    python3 - "$scratch/wal/1.wal" <<'PY'
import struct, sys
def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF
payload = b'Q0'
open(sys.argv[1], 'wb').write(struct.pack('<II', len(payload), crc32c(payload)) + payload)
PY
}
configure() {
    rm -rf "$scratch/wal" "$scratch/archive" "$scratch"/catalog.sqlite* "$scratch/out" "$scratch"/*.log
    mkdir -p "$scratch/out" "$scratch/archive"
    [ "$mode" != unbounded ] || seed_wal_without_policy
    cat > "$scratch/visor.json" <<JSON
{"listen":"127.0.0.1:$port","internal_listen":"127.0.0.1:$((port+1))","db_path":"$scratch/catalog.sqlite","keepers":[{"process_id":"keeper-1","endpoint":"127.0.0.1:$((port+2))"}],"graphers":["127.0.0.1:$gport"],"player":"127.0.0.1:$((port+4))"}
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
    start visor "$visor" 'catalog ready' && start keeper "$keeper" 'journal ready' &&
        start grapher "$grapher" 'grapher ready' && start player "$player" 'player ready'
}
dump() { tail -n 40 "$scratch"/*.log 2>/dev/null || true; }
step() {
    local name=$1
    shift
    if ! timeout 120 "$@"; then
        echo "FAIL $mode $name"
        dump
        exit 1
    fi
}
for mode in unbounded complete; do
    # Ports stay below the kernel ephemeral range; a launch that loses a bind race retries on fresh ports.
    for attempt in 1 2 3; do
        port=$((10000 + (RANDOM % 4400) * 5))
        gport=$((10000 + (RANDOM % 4400) * 5))
        [ "$gport" -ne "$port" ] || continue
        if launch; then break; fi
        dump
        stop
        [ "$attempt" -lt 3 ] || exit 1
    done
    out="$scratch/out"
    step write "$driver" policy-write "127.0.0.1:$port" "$out"
    step read "$driver" policy-read "127.0.0.1:$port" "127.0.0.1:$((port+3))" "$out" "$mode"
    stop
done
echo "POLICY MARKER PASSED"
