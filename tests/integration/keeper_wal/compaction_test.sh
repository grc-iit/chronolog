#!/usr/bin/env bash
# Real-process compaction, stale Player plans, Tail continuity and destroy (I13.11, I13.12).
set -eu
visor=$1 keeper=$2 grapher=$3 player=$4 driver=$5 barrier_library=$6
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
    # An instrumented executable requires its sanitizer runtime first in the preload list.
    local sanitizer
    sanitizer=$(ldd "$binary" | awk '/libasan.so|libtsan.so/ {print $3; exit}')
    env COMPACTION_BARRIER="$scratch" LD_PRELOAD="${sanitizer:+$sanitizer:}$barrier_library${LD_PRELOAD:+:$LD_PRELOAD}" "$binary" --config "$scratch/$role.json" > "$scratch/$log.log" 2>&1 &
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
{"process_id":"grapher-1","internal_listen":"127.0.0.1:$gport","self_endpoint":"127.0.0.1:$gport","visor_internal":"127.0.0.1:$((port+1))","archive_root":"$scratch/archive","heartbeat_interval_ms":200,"manifest_writer":"compaction-gate","compact_enabled":false,"compact_scan_interval_secs":5,"compact_min_age_secs":5,"compact_min_files":2,"compact_max_files":128}
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
# Each compaction observation has a bounded 2*(scan interval + minimum age) = 20 s allowance.
# These are progress bounds, not performance assertions; no transition is inferred from a sleep.
await() {
    local name=$1
    shift
    for _ in $(seq 1 400); do
        if "$@"; then return; fi
        sleep 0.05
    done
    echo "FAIL $name"
    dump
    cat "$scratch/archive/manifest/compaction-gate.log" 2>/dev/null || true
    exit 1
}
reap() {
    local role=$1
    if ! wait "${pid_of[$role]}"; then echo "FAIL $role"; dump; exit 1; fi
    unset 'pid_of[$role]'
}
restart_grapher() {
    kill -KILL "${pid_of[grapher]}"
    wait "${pid_of[grapher]}" 2>/dev/null || true
    unset 'pid_of[grapher]'
    start grapher "$1" "$grapher" 'grapher ready' || { echo "FAIL grapher restart"; dump; exit 1; }
}
files() { find "$1" -maxdepth 1 -type f -name '*.h5' | wc -l; }
manifest="$scratch/archive/manifest/compaction-gate.log"
step seed "$driver" compact-write "$catalog" "$out" "$scratch/archive" events
story=$(cat "$out/story")
step before "$driver" compact-read "$catalog" "$keeper_internal" "$out"
before=$(files "$scratch/archive/$story")
[ "$before" -ge 8 ] || { echo 'FAIL insufficient archive inputs'; exit 1; }
timeout 100 "$driver" compact-tail "$catalog" "$out" > "$scratch/tail.log" 2>&1 &
pid_of[tail]=$!
await 'Tail did not start before compaction' test -s "$out/tail.ready"
printf '%s/' "$scratch/archive/$story" > "$scratch/read.arm"
timeout 60 "$driver" compact-read "$catalog" "$keeper_internal" "$out" > "$scratch/crossing.log" 2>&1 &
pid_of[crossing]=$!
await 'Player did not reach its planned archive open' test -s "$scratch/read.ready"
kill -0 "${pid_of[crossing]}" || { echo 'FAIL crossing Read completed before the switch'; exit 1; }
sed -i 's/"compact_enabled":false/"compact_enabled":true/' "$scratch/grapher.json"
restart_grapher grapher-compact
await 'no committed compact_v1 line' grep -q '"compact_v1"' "$manifest"
blocked=$(cat "$scratch/read.ready")
await 'planned input was not retired by compaction' test ! -e "$blocked"
fewer_files() { [ "$(files "$scratch/archive/$story")" -lt "$before" ]; }
await 'archive count did not drop' fewer_files
after=$(files "$scratch/archive/$story")
touch "$scratch/read.release"
reap crossing
step after "$driver" compact-read "$catalog" "$keeper_internal" "$out"
step sentinel "$driver" compact-end "$catalog" "$out"
reap tail
# Seed the second story with compaction disabled, then stop inside a real job's temporary creation.
sed -i 's/"compact_enabled":true/"compact_enabled":false/' "$scratch/grapher.json"
restart_grapher grapher-seed
mkdir "$scratch/doomed"
step doomed-seed "$driver" compact-write "$catalog" "$scratch/doomed" "$scratch/archive" doomed
doomed=$(cat "$scratch/doomed/story")
printf '%s/.compact-' "$scratch/archive/$doomed" > "$scratch/compact.arm"
sed -i 's/"compact_enabled":false/"compact_enabled":true/' "$scratch/grapher.json"
restart_grapher grapher-destroy
await 'destroy story never entered compaction' test -s "$scratch/compact.ready"
temporary=$(cat "$scratch/compact.ready")
[ -f "$temporary" ] || { echo 'FAIL no in-progress compaction temporary'; exit 1; }
step destroy "$driver" compact-destroy "$catalog" "$scratch/doomed"
# The manifest tombstone proves the erase is ordered before the paused job resumes.
await 'destroy tombstone not recorded' grep -q '"story":'"$doomed"',"tombstoned":true' "$manifest"
touch "$scratch/compact.release"
empty_story() { [ -z "$(find "$scratch/archive/$doomed" -type f -print -quit)" ]; }
await 'I13.11 left story files on disk' empty_story
# Wait for the actual paused job to finish as well, so a transient empty directory cannot pass.
await 'paused compaction did not abort after destroy' grep -q 'story tombstoned during compaction\|No such file or directory' "$scratch/grapher-destroy.log"
empty_story || { echo 'FAIL compaction recreated destroyed story files'; dump; exit 1; }
echo "COMPACTION PASSED files=$before->$after exact_reads=before,crossing,after exact_tail=once destroy_files=0"
