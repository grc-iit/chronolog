#!/usr/bin/env bash
# Runs a ChronoLog benchmark suite and writes JSON lines outside git.
#   run.sh micro|append|replay|tail|archive|all [--quick] [--perf] [--binaries DIR] [--out DIR]
# On dragon run it through rbuild so the build lock is held and nothing else shares the machine:
#   BENCH=...; rbuild "BENCH_COMMIT=$(git rev-parse --short HEAD) bash tools/bench/run.sh all --perf"
# Results land in ~/chronolog-sprint/bench/<commit>/ unless --out is given. --quick uses reduced parameters
# and exists only to prove the harness works (ctest label bench); it asserts no number.
set -uo pipefail

suite=${1:-all}
shift || true
quick=0 perf=0 binaries="" out=""
while [ $# -gt 0 ]; do
    case "$1" in
        --quick) quick=1 ;;
        --perf) perf=1 ;;
        --binaries) binaries=$2; shift ;;
        --out) out=$2; shift ;;
        *) echo "run.sh: unknown option $1"; exit 2 ;;
    esac
    shift
done
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
binaries=${binaries:-$root/build/bench}
commit=${BENCH_COMMIT:-unknown}
out=${out:-$HOME/chronolog-sprint/bench/$commit}
mkdir -p "$out"
walbase=${BENCH_WAL_DIR:-$HOME/chronolog-sprint/bench/tmp}
mkdir -p "$walbase"
scratch=$(mktemp -d "$walbase/run.XXXXXX")

visor=$binaries/src/chrono-visor/chrono_visor
keeper=$binaries/src/chrono-keeper/chrono_keeper
grapher=$binaries/src/chrono-grapher/server/chrono_grapher
player=$binaries/src/chrono-player/chrono_player
micro=$binaries/tools/bench/chronolog_bench_micro
load=$binaries/tools/bench/chronolog_bench_load

pids=()
stop_stack() {
    for pid in "${pids[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    sleep 0.5
    for pid in "${pids[@]}"; do kill -KILL "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    pids=()
}
trap 'stop_stack; rm -rf "$scratch"' EXIT

python3 "$here/records.py" meta --binaries "$binaries" --wal-dir "$walbase" --quick "$quick" \
    --build-lock "${RBUILD_HELD:-none}" --perf "$perf" > "$out/meta.json" || exit 1

start_one() {
    local role=$1 ready=$2 binary=$3
    "$binary" --config "$scratch/$role.json" > "$scratch/$role.log" 2>&1 &
    pids+=("$!")
    for _ in $(seq 1 300); do
        if grep -q "$ready" "$scratch/$role.log"; then return 0; fi
        kill -0 "${pids[-1]}" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

# start_stack RETENTION_CAP_MB TAIL_POLL_MS. Ports follow rules.md: random block, offsets +0..+4, a fresh block on a bind race.
start_stack() {
    local cap=$1 poll=$2 attempt
    for attempt in 1 2 3; do
        port=$((10000 + (RANDOM % 4400) * 5))
        gport=$((10000 + (RANDOM % 4400) * 5))
        [ "$port" != "$gport" ] || continue
        rm -rf "$scratch/wal" "$scratch/archive" "$scratch"/catalog.sqlite*
        mkdir -p "$scratch/archive"
        cat > "$scratch/visor.json" <<JSON
{"listen":"127.0.0.1:$port","internal_listen":"127.0.0.1:$((port+1))","db_path":"$scratch/catalog.sqlite","keepers":[{"process_id":"keeper-1","endpoint":"127.0.0.1:$((port+2))"}],"grapher":"127.0.0.1:$gport","player":"127.0.0.1:$((port+4))"}
JSON
        cat > "$scratch/keeper.json" <<JSON
{"listen":"127.0.0.1:$((port+2))","internal_listen":"127.0.0.1:$((port+3))","self_endpoint":"127.0.0.1:$((port+2))","visor_internal":"127.0.0.1:$((port+1))","wal_dir":"$scratch/wal","worker_threads":4,"heartbeat_interval_ms":100,"story_chunk_duration_secs":1,"seal_interval_ms":200,"archive_visibility_delay_secs":1,"shutdown_confirm_timeout_secs":1,"retention_cap_mb":$cap,"wal_max_bytes":8589934592}
JSON
        cat > "$scratch/grapher.json" <<JSON
{"process_id":"grapher-1","internal_listen":"127.0.0.1:$gport","self_endpoint":"127.0.0.1:$gport","visor_internal":"127.0.0.1:$((port+1))","archive_root":"$scratch/archive","heartbeat_interval_ms":200}
JSON
        cat > "$scratch/player.json" <<JSON
{"listen":"127.0.0.1:$((port+4))","advertise":"127.0.0.1:$((port+4))","visor":"127.0.0.1:$port","visor_internal":"127.0.0.1:$((port+1))","keeper_internal":{"keeper-1":"127.0.0.1:$((port+3))"},"tail_poll_ms":$poll,"manifest_poll_ms":100,"archive_root":"$scratch/archive"}
JSON
        if start_one visor 'catalog ready' "$visor" && start_one keeper 'journal ready' "$keeper" \
            && start_one grapher 'grapher ready' "$grapher" && start_one player 'player ready' "$player"; then
            return 0
        fi
        cat "$scratch"/*.log 2>/dev/null | tail -20
        stop_stack
    done
    echo "run.sh: the stack did not start after 3 attempts"
    return 1
}

# Starts perf on the service processes when --perf is set; stop_perf folds the profile into profiles.jsonl.
perf_pids=()
roles=(visor keeper grapher player)
start_perf() {
    [ "$perf" = 1 ] || return 0
    local name=$1 i
    perf_pids=()
    for i in "${!pids[@]}"; do
        perf record -q -F 499 -g --call-graph fp -p "${pids[$i]}" -o "$scratch/perf-$name-${roles[$i]}.data" \
            > "$scratch/perf-$name-${roles[$i]}.log" 2>&1 &
        perf_pids+=("$!")
    done
    sleep 0.5
}
stop_perf() {
    [ "$perf" = 1 ] && [ "${#perf_pids[@]}" -gt 0 ] || return 0
    local name=$1 i spec=""
    for pid in "${perf_pids[@]}"; do kill -INT "$pid" 2>/dev/null; done
    for pid in "${perf_pids[@]}"; do wait "$pid" 2>/dev/null; done
    perf_pids=()
    for i in "${!roles[@]}"; do spec+="${roles[$i]}=$scratch/perf-$name-${roles[$i]}.data,"; done
    python3 "$here/records.py" profile --data "${spec%,}" --workload "$name" --meta "$out/meta.json" \
        >> "$out/profiles.jsonl" 2> "$scratch/perf-$name.err" \
        || echo "run.sh: no profile for $name: $(tail -2 "$scratch/perf-$name.err")"
    [ -f "$out/profile-$name.txt" ] && cat "$out/profile-$name.txt"
}

# run_load SCENARIO [--key value ...] writes raw lines; records.py adds the metadata.
run_load() {
    "$load" "$@" --catalog "127.0.0.1:$port" --player "127.0.0.1:$((port+4))" --keeper-log "$scratch/keeper.log" \
        --archive-root "$scratch/archive" --out "$out/raw-$current.jsonl"
}

finish() {
    local name=$1
    python3 "$here/records.py" wrap --raw "$out/raw-$name.jsonl" --meta "$out/meta.json" >> "$out/$name.jsonl" \
        && rm -f "$out/raw-$name.jsonl"
}

suite_micro() {
    local args=(--benchmark_format=json --benchmark_out="$scratch/micro.json" --benchmark_out_format=json)
    [ "$quick" = 1 ] && args+=(--benchmark_min_time=0.01s)
    CHRONOLOG_BENCH_WAL_DIR="$walbase" "$micro" "${args[@]}" > "$scratch/micro.stdout" || { cat "$scratch/micro.stdout"; return 1; }
    python3 "$here/records.py" gbench --in "$scratch/micro.json" --meta "$out/meta.json" >> "$out/micro.jsonl"
}

suite_append() {
    current=append
    start_stack 4096 200 || return 1
    local payloads="64 1024 65536" batches="1 64" writers="1 16" seconds=5 warmup=1 cell=0 p b w d
    if [ "$quick" = 1 ]; then payloads=1024; batches=1; writers=1; seconds=1; warmup=0.2; fi
    for p in $payloads; do for b in $batches; do for w in $writers; do for d in accepted durable; do
        cell=$((cell+1))
        run_load append --payload "$p" --batch "$b" --writers "$w" --durability "$d" --seconds "$seconds" \
            --warmup "$warmup" --cell "$cell" || return 1
    done; done; done; done
    if [ "$perf" = 1 ]; then
        start_perf append
        run_load append --payload 1024 --batch 64 --writers 16 --durability durable --seconds 10 --warmup 1 --cell profile > /dev/null
        stop_perf append
    fi
    stop_stack
    finish append
}

suite_replay() {
    current=replay
    local events=100000 profile cap
    [ "$quick" = 1 ] && events=3000
    for profile in hot cold; do
        cap=4096
        [ "$profile" = cold ] && cap=8
        start_stack "$cap" 200 || return 1
        [ "$perf" = 1 ] && [ "$profile" = hot ] && start_perf replay
        run_load replay --events "$events" --payload 1024 --profile "$profile" || return 1
        [ "$perf" = 1 ] && [ "$profile" = hot ] && stop_perf replay
        stop_stack
    done
    finish replay
}

suite_tail() {
    current=tail
    local events=2000 rate=200 poll
    [ "$quick" = 1 ] && events=100 && rate=100
    for poll in 200 10; do
        start_stack 4096 "$poll" || return 1
        [ "$perf" = 1 ] && [ "$poll" = 10 ] && start_perf tail
        run_load tail --events "$events" --rate "$rate" --payload 1024 --tail-poll-ms "$poll" || return 1
        [ "$perf" = 1 ] && [ "$poll" = 10 ] && stop_perf tail
        stop_stack
    done
    finish tail
}

suite_archive() {
    current=archive
    local events=100000
    [ "$quick" = 1 ] && events=3000
    start_stack 4096 200 || return 1
    start_perf archive
    run_load archive --events "$events" --payload 1024 || return 1
    stop_perf archive
    stop_stack
    finish archive
}

rc=0
case "$suite" in
    micro|append|replay|tail|archive) "suite_$suite" || rc=1 ;;
    all) for s in micro append replay tail archive; do "suite_$s" || { echo "run.sh: suite $s failed"; rc=1; }; done ;;
    *) echo "usage: run.sh micro|append|replay|tail|archive|all [--quick] [--perf] [--binaries DIR] [--out DIR]"; exit 2 ;;
esac
echo "results in $out (build lock: ${RBUILD_HELD:-none})"
exit "$rc"
