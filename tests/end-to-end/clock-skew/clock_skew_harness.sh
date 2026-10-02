#!/usr/bin/env bash
# Clock skew gate (section 8): a client whose clock is skewed measures its offset to the Visor authority within the
# uncertainty it reports, so two differently skewed clients land on one timeline. Cluster.ReadClock stays as
# observability; skew is injected through the SDK TimeSource seam, never through a daemon environment hook.
# The oracle is the legacy one: |measured offset - (-skew)| <= reported uncertainty (RTT/2) + 2 ms of slack, which
# the probe asserts for every exchange.
#   Scenario 1: heterogeneous skewed clients against the real Visor.
#   Scenario 2: a skewed client still appends through a real Keeper, and the daemons stay alive without
#               register or heartbeat failures.
#   Scenario 3: a mid-run clock step is recovered by the next exchange.
# The legacy daemon-skew scenarios have no 4.0 counterpart: daemons no longer exchange clocks with the Visor, the
# Keeper assigns hlc from its own chrony-backed clock, and ReadClock must never feed it (section 8).
set -eu
visor=$1 keeper=$2 probe=$3
scratch=$(mktemp -d)
pids=()
trap 'for pid in "${pids[@]}"; do kill -KILL "$pid" 2>/dev/null || true; done; rm -rf "$scratch"' EXIT
dump() { tail -n 30 "$scratch"/*.log 2>/dev/null || true; }
start() {
    local name=$1 binary=$2 ready=$3
    "$binary" --config "$scratch/$name.json" > "$scratch/$name.log" 2>&1 &
    pids+=("$!")
    for _ in $(seq 1 300); do
        if grep -q "$ready" "$scratch/$name.log"; then return 0; fi
        kill -0 "$!" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
launch() {
    cat > "$scratch/visor.json" <<JSON
{"membership_mode":"dynamic","listen":"127.0.0.1:$port","internal_listen":"127.0.0.1:$((port+1))","db_path":"$scratch/catalog.sqlite","keepers":[{"process_id":"keeper-1","endpoint":"127.0.0.1:$((port+3))"}],"graphers":["127.0.0.1:$((port+5))"],"player":"127.0.0.1:$((port+6))","keeper_failure_timeout_ms":5000,"release_fence_timeout_ms":1000,"worker_threads":4,"raft":{"server_id":1,"raft_endpoint":"127.0.0.1:$((port+2))","peers":[{"id":1,"raft_endpoint":"127.0.0.1:$((port+2))","catalog_endpoint":"127.0.0.1:$port","internal_endpoint":"127.0.0.1:$((port+1))"}],"election_lower_ms":300,"election_upper_ms":600}}
JSON
    cat > "$scratch/keeper.json" <<JSON
{"process_id":"keeper-1","listen":"127.0.0.1:$((port+3))","internal_listen":"127.0.0.1:$((port+4))","self_endpoint":"127.0.0.1:$((port+3))","visor_internal":"127.0.0.1:$((port+1))","wal_dir":"$scratch/wal","worker_threads":4,"heartbeat_interval_ms":100,"append_ceiling_wait_ms":500}
JSON
    # The Keeper registers once the Visor holds the leader lease, which ReadClock needs too.
    start visor "$visor" 'catalog ready' && timeout 30 "$probe" offset "127.0.0.1:$((port+1))" 0 > /dev/null &&
        start keeper "$keeper" 'journal ready'
}
for attempt in 1 2 3; do
    port=$((10000 + (RANDOM % 4400) * 5))
    rm -rf "$scratch/wal" "$scratch"/catalog.sqlite*
    if launch; then break; fi
    dump
    for pid in "${pids[@]}"; do kill -KILL "$pid" 2>/dev/null || true; done
    pids=()
    [ "$attempt" -lt 3 ] || exit 1
done
internal="127.0.0.1:$((port+1))"
catalog="127.0.0.1:$port"
failures=0
run() {
    echo "-- $*"
    if ! timeout 60 "$probe" "$@"; then failures=$((failures + 1)); fi
}
echo "## Scenario 1: heterogeneous skewed clients, one Visor timeline"
run offset "$internal" 120000000
run offset "$internal" -75000000
echo "## Scenario 2: skewed writers through a real Keeper, daemons stay healthy"
run write "$catalog" "$internal" 50000000
run write "$catalog" "$internal" -30000000
echo "## Scenario 3: mid-run step recovered by the next exchange"
run offset "$internal" 20000000 300000000
for pid in "${pids[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then echo "PASS daemon $pid alive"; else echo "FAIL daemon $pid died"; failures=$((failures + 1)); fi
done
if grep -qE 'register failed|heartbeat failed' "$scratch/keeper.log"; then
    echo "FAIL the Keeper logged register or heartbeat failures under skew"
    failures=$((failures + 1))
else
    echo "PASS no register or heartbeat failures under skew"
fi
if [ "$failures" -ne 0 ]; then
    dump
    echo "CLOCK SKEW FAILED failures=$failures"
    exit 1
fi
echo "CLOCK SKEW PASSED"
