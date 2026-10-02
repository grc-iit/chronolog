#!/usr/bin/env bash
# Keeper crash gate (I5.6, I12 Keeper crash, I12.1): SIGKILL a Keeper whose retained chunks are unsettled,
# restart it on the same WAL, and prove replay, HLC resumption and delivery to the archive on a real stack.
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
catalog="127.0.0.1:$port"
keeper_internal="127.0.0.1:$((port+3))"
out="$scratch/out"
step() {
    local name=$1
    shift
    if ! timeout 120 "$@"; then
        echo "FAIL $name"
        dump
        exit 1
    fi
}
# The Player opens the archive the Grapher created. With the Grapher gone, every sealed chunk stays unsettled.
kill -KILL "${pid_of[grapher]}"
wait "${pid_of[grapher]}" 2>/dev/null || true
unset 'pid_of[grapher]'
step write "$driver" write "$catalog" "$out"
for _ in $(seq 1 300); do
    grep -q 'archive_transfer_start' "$scratch/keeper.log" && break
    sleep 0.1
done
grep -q 'archive_transfer_start' "$scratch/keeper.log" || { echo "FAIL no sealed chunk reached the transfer stage"; dump; exit 1; }
grep -o 'archive_transfer_start chunk=[^ ]*' "$scratch/keeper.log" | sed 's/.*chunk=//' | sort -u > "$out/chunks"
step frontier "$driver" frontier "$keeper_internal" "$out"
kill -KILL "${pid_of[keeper]}"
wait "${pid_of[keeper]}" 2>/dev/null || true
unset 'pid_of[keeper]'
start keeper keeper2 "$keeper" 'journal ready' || { echo "FAIL keeper restart"; dump; exit 1; }
step append "$driver" append "$catalog" "$out"
step resume "$driver" after "$out"
step hot "$driver" hot "$keeper_internal" "$out"
start grapher grapher2 "$grapher" 'grapher ready' || { echo "FAIL grapher restart"; dump; exit 1; }
step verify "$driver" verify "$catalog" "$keeper_internal" "$out"
while read -r chunk; do
    grep -q "archive_published chunk=$chunk " "$scratch/grapher2.log" || { echo "FAIL chunk $chunk was not resent to the archive"; dump; exit 1; }
done < "$out/chunks"
echo "KEEPER WAL RESTART PASSED chunks=$(wc -l < "$out/chunks")"
