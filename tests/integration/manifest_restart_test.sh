#!/usr/bin/env bash
# Archive manifest across a Player restart (I13.5, section 13): a Player restarted on a populated archive must build
# its index from the manifest and return the same complete read as before. Only the Player restarts, so nothing
# repopulates the index through the ordinary runtime path.
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
step write "$driver" write "$catalog" "$out"
# verify waits until the Keeper evicted every durable event, so the read below is served by the archive alone.
step before "$driver" verify "$catalog" "$keeper_internal" "$out" "$out/before.tsv"
kill -KILL "${pid_of[player]}"
wait "${pid_of[player]}" 2>/dev/null || true
unset 'pid_of[player]'
start player player2 "$player" 'player ready' || { echo "FAIL player restart"; dump; exit 1; }
records=$(sed -n 's/.*archive index loaded from manifest records=\([0-9][0-9]*\).*/\1/p' "$scratch/player2.log" | head -n 1)
[ -n "$records" ] && [ "$records" -gt 0 ] || { echo "FAIL the restarted Player did not report an index built from a populated manifest"; dump; exit 1; }
step after "$driver" verify "$catalog" "$keeper_internal" "$out" "$out/after.tsv"
[ -s "$out/before.tsv" ] && cmp -s "$out/before.tsv" "$out/after.tsv" || { echo "FAIL the complete read changed across the Player restart"; diff "$out/before.tsv" "$out/after.tsv" | head; exit 1; }
echo "MANIFEST RESTART PASSED manifest_records=$records events=$(wc -l < "$out/after.tsv")"
