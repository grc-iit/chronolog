#!/usr/bin/env bash
#
# Tier-2 clock-skew harness for the Visor-based clock (see visor_clock_exchange.md).
#
# Runs the REAL ChronoLog binaries (visor/keeper/grapher/player + chrono-bench
# clients) over real Thallium RPC, but injects a per-process monotonic-clock skew
# via the CHRONOLOG_SIM_CLOCK_OFFSET_NS / CHRONOLOG_SIM_CLOCK_STEP_FILE hook baked
# into ChronoClock::ProcessClock() (a test-only affordance, inert when unset).
#
# This is the level single-node testing normally can't reach: on one host every
# process shares the steady_clock boot epoch, so without the injected skew the
# offset is ~0. steady_clock is CLOCK_MONOTONIC and not settable by date/NTP/
# libfaketime-default, so the env hook is how we skew what ChronoLog actually reads.
#
# Oracle: a node skewed by S measures offset ~= -S (it maps its skewed local clock
# back onto the Visor timeline). The measured offset must land within the node's
# OWN reported uncertainty (== RTT/2) of -S — that IS the design guarantee
# (|error| <= +/- RTT/2), rather than a fixed tolerance. Two differently-skewed
# clients therefore both land on the Visor timeline, i.e. their event ChronoTicks
# are comparable — the headline property.
#
# NOTE: the busy Keeper's heartbeat round trip is markedly slower/asymmetric than
# the Grapher/Player's (uncertainty ~50 ms vs ~sub-ms), so its clock is only
# accurate to ~tens of ms. That is well inside the chunk-decay acceptance window,
# but is a real per-component signal this harness surfaces (see the harness output
# and visor_clock_exchange.md §6.3).
#
# Assumes a Debug build has been installed (local_single_user_deploy.sh -i).
# Usage:  tests/end-to-end/clock-skew/clock_skew_harness.sh

set -uo pipefail

INSTALL_DIR="${CHRONOLOG_INSTALL_DIR:-$HOME/chronolog-install/chronolog}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
MPIEXEC="${MPIEXEC:-$REPO_ROOT/.spack-env/view/bin/mpiexec}"
BENCH="$INSTALL_DIR/tools/benchmark/chrono-bench"
CLIENT_CONF="$INSTALL_DIR/conf/chrono-client-conf.json"
CLIENT_LOG="$INSTALL_DIR/monitor/chrono-client.log"
DEPLOY="$INSTALL_DIR/tools/deploy/deploy_local.sh"
MON="$INSTALL_DIR/monitor"
SLACK_NS=2000000     # 2 ms slack on top of the reported uncertainty bound
NOTE_UNC_NS=10000000 # flag (soft NOTE) when a node's uncertainty exceeds 10 ms
STEP_FILE="$(mktemp -t chronolog_sim_step.XXXXXX)"

pass=0
fail=0
ok() {
    echo "  PASS: $*"
    pass=$((pass + 1))
}
bad() {
    echo "  FAIL: $*"
    fail=$((fail + 1))
}

kill_cluster() {
    pkill -9 -f "chrono-(visor|keeper|grapher|player) --config" 2>/dev/null
    sleep 2
}
cleanup() {
    kill_cluster
    rm -f "$STEP_FILE"
}
trap cleanup EXIT

abs() { local v=$1; echo $((v < 0 ? -v : v)); }

# Newest log file for a role (deploys re-number logs; pick by mtime, skip launch).
newest_log() { ls -t "$MON"/chrono-"$1"-*.log 2>/dev/null | grep -v '\.launch\.log' | head -1; }

# Echo "offset uncertainty" (two ints) from the last clock-sync line of a log file.
parse_off_unc() {
    grep -oE 'offset=-?[0-9]+, uncertainty=[0-9]+' "$1" 2>/dev/null | tail -1 \
        | grep -oE -- '-?[0-9]+' | tr '\n' ' '
}

# Assert the measured offset is within the reported uncertainty of expected.
#   $1 logfile   $2 expected_ns   $3 label
assert_offset() {
    local off unc
    read -r off unc < <(parse_off_unc "$1")
    if [ -z "${off:-}" ]; then bad "$3: no clock-sync logged"; return; fi
    local err bound
    err="$(abs $((off - $2)))"
    bound=$((unc + SLACK_NS))
    if [ "$err" -le "$bound" ]; then
        ok "$3: offset=$off (expected ~$2, err=$err <= uncertainty $unc)"
    else
        bad "$3: offset=$off (expected ~$2, err=$err > uncertainty $unc + slack)"
    fi
    [ "$unc" -gt "$NOTE_UNC_NS" ] && echo "  NOTE: $3 heartbeat uncertainty is large (${unc} ns ~ RTT/2)"
}

run_skewed_client() { # skew_ns n_events
    : >"$CLIENT_LOG"
    CHRONOLOG_SIM_CLOCK_OFFSET_NS="$1" "$MPIEXEC" -n 1 "$BENCH" -c "$CLIENT_CONF" \
        -w -n "$2" -t 1 -h 1 -a 4096 -s 4096 -b 4096 -p >/dev/null 2>&1
}

check_client_skew() { # skew_ns
    run_skewed_client "$1" 5
    assert_offset "$CLIENT_LOG" "$((-1 * $1))" "client skew=$1 ns"
}

echo "############################################################"
echo "# Scenario 1: heterogeneous skewed clients, unskewed cluster"
echo "############################################################"
kill_cluster
"$DEPLOY" -d -w "$INSTALL_DIR" -k 1 -r 1 >/dev/null 2>&1
sleep 4
check_client_skew 120000000  # +120 ms (client clock ahead)
check_client_skew -75000000  # -75 ms  (client clock behind)
kill_cluster

echo "############################################################"
echo "# Scenario 2: uniformly skewed daemons stay healthy and sync"
echo "############################################################"
kill_cluster
DAEMON_SKEW=200000000 # +200 ms on every daemon; the Visor authority (VisorClock,
                      # raw steady_clock) is unaffected, so it stays the reference
CHRONOLOG_SIM_CLOCK_OFFSET_NS="$DAEMON_SKEW" "$DEPLOY" -d -w "$INSTALL_DIR" -k 1 -r 1 >/dev/null 2>&1
sleep 14 # allow at least one heartbeat/clock-exchange cycle
exp=$((-1 * DAEMON_SKEW))
for role in keeper grapher player; do
    assert_offset "$(newest_log "$role")" "$exp" "$role daemon skew=$DAEMON_SKEW ns"
done
# a skewed client on top of the skewed cluster still writes and syncs
check_client_skew 50000000
# health: daemons alive, no RPC timeouts
alive=0
for r in visor keeper grapher player; do alive=$((alive + $(pgrep -cf "chrono-$r --config"))); done
[ "$alive" -ge 4 ] && ok "all 4 daemons alive under skew" || bad "only $alive/4 daemons alive under skew"
if grep -qhE "Stats/clock-sync RPC timed out" "$MON"/chrono-*-*.log 2>/dev/null; then
    bad "heartbeat RPC timeouts observed under skew"
else
    ok "no heartbeat RPC timeouts under skew"
fi
kill_cluster

echo "############################################################"
echo "# Scenario 3: mid-run clock step recovered by re-sync"
echo "############################################################"
kill_cluster
echo 0 >"$STEP_FILE" # start with no step
CHRONOLOG_SIM_CLOCK_STEP_FILE="$STEP_FILE" "$DEPLOY" -d -w "$INSTALL_DIR" -k 1 -r 1 >/dev/null 2>&1
sleep 12
assert_offset "$(newest_log keeper)" 0 "keeper before step"
echo 300000000 >"$STEP_FILE" # mid-run: step the daemon clocks +300 ms
sleep 14                     # step file re-read (<=1s) + next heartbeat re-sync
assert_offset "$(newest_log keeper)" -300000000 "keeper after +300 ms mid-run step"
kill_cluster

echo "############################################################"
echo "# Result: $pass passed, $fail failed"
echo "############################################################"
[ "$fail" -eq 0 ]
