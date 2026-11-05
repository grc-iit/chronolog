#!/bin/bash
# Integration test for SIGTERM while a durable write is inside its group sync.
#
# One keeper, WAL on. libchronolog_wal_sync_hold.so is preloaded and holds
# fdatasync of a WAL segment while a file exists. One durable write is sent
# and observed inside that sync. The keeper is then sent SIGTERM and the hold
# is released, so the sync can finish and the write can be answered. A later
# durable write must get CL_ERR_DURABILITY_UNAVAILABLE and not be stored.
# Every write that was answered CL_SUCCESS is still returned, exactly once,
# and the keeper's exit-summary reports no events left unconfirmed.
#
#   1. The first durable write is inside fdatasync before SIGTERM.
#   2. That write is answered CL_SUCCESS after the hold is released.
#   3. A record_event_durable sent to the keeper after it has stopped taking
#      writes, and while the held sync is still what keeps the process up,
#      gets CL_ERR_DURABILITY_UNAVAILABLE, and the RPC is answered within 2 s
#      (the probe times the RPC, not its own start-up). The visor has already
#      been told the keeper is gone, so this call does not go through
#      AcquireStory.
#   4. The keeper exits and leaves an exit-summary. The one event is still
#      unconfirmed, or the grapher confirmed it during shutdown; either count
#      is reported, and the replay below still returns that event.
#   5. After the keeper is started again, a replay returns every identity
#      that was answered CL_SUCCESS, exactly once, and not the refused write
#      from check 3, so that write stored nothing.
#
# The preload stalls only a "<20 digits>.wal" file, and only while the hold
# file exists, so startup and the exit summary are not held. Requires an
# installed tree and the build tree for the probe and the preload. The script
# patches the installed conf template, deploys 1 keeper / 1 recording group,
# stops the deployment and restores the template.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        ./keeper_wal_shutdown_test.sh

set -u

WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
BUILD_DIR="${BUILD_DIR:-$HOME/chronolog-build/Debug}"

MONITOR_DIR="$WORK_DIR/monitor"
OUTPUT_DIR="$WORK_DIR/output"
CONF_TEMPLATE="$WORK_DIR/conf/default-chrono-conf.json"
CLIENT_CONF="$WORK_DIR/conf/default-chrono-client-conf.json"
KEEPER_CONF="$WORK_DIR/conf/chrono-keeper-conf-1.json"
DEPLOY="$WORK_DIR/tools/deploy/deploy_local.sh"
PROBE="$BUILD_DIR/tests/integration/keeper-wal/chronolog-test-durable-probe"
HOLD_LIB="$BUILD_DIR/tests/integration/keeper-wal/libchronolog_wal_sync_hold.so"
WAL_DIR="$(realpath -m "$WORK_DIR/wal/keeper-1")"

CHRONICLE=KeeperWalShutdownChronicle
STORY=ShutdownStory
K_CHUNK_SECS=2
K_ACCEPT_SECS=3
G_CHUNK_SECS=5
G_ACCEPT_SECS=5
REPORT_SECS=1
VISIBILITY_SECS=1
SHUTDOWN_CONFIRM_SECS=60

PASS=0
FAIL=0

say() { echo -e "[keeper_wal_shutdown_test $(date +%H:%M:%S)] $*"; }
ok() { say "PASS: $*"; PASS=$((PASS + 1)); }
bad() { say "FAIL: $*"; FAIL=$((FAIL + 1)); }

kill_daemons() {
    for bin in chrono-visor chrono-keeper chrono-grapher chrono-player; do
        pkill -9 -f "$WORK_DIR/bin/$bin" 2> /dev/null
    done
}

cleanup() {
    # a keeper still inside the held sync has to be released before stop waits
    # for it to finish shutting down
    rm -f "${HOLD:-}"
    "$DEPLOY" -s -w "$WORK_DIR" > /dev/null 2>&1
    kill_daemons
}

keeper_pid() { pgrep -f "$WORK_DIR/bin/chrono-keeper --config" | head -1; }

identities() { grep -E '^[0-9]+ [0-9]+ [0-9]+$' "$1" | sort -u; }

client_conf() {
    jq ".chrono_client.ClientQueryService.rpc.service_base_port = $2" \
        "$CLIENT_CONF" > "$RUN_DIR/$1_client_conf.json" || { say "$1 client conf patch failed"; exit 2; }
}

command -v jq > /dev/null || { say "jq not found"; exit 2; }
[ -x "$PROBE" ] || { say "probe not found at $PROBE"; exit 2; }
[ -f "$HOLD_LIB" ] || { say "preload not found at $HOLD_LIB"; exit 2; }
[ -x "$DEPLOY" ] || { say "deploy script not found at $DEPLOY"; exit 2; }

say "patching installed conf template: keeper ${K_CHUNK_SECS}s/${K_ACCEPT_SECS}s chunks, shutdown confirm ${SHUTDOWN_CONFIRM_SECS}s"
cp "$CONF_TEMPLATE" "$CONF_TEMPLATE.wal_shutdown_backup"
jq ".chrono_keeper.DataStoreInternals.story_chunk_duration_secs = $K_CHUNK_SECS |
    .chrono_keeper.DataStoreInternals.acceptance_window_secs = $K_ACCEPT_SECS |
    .chrono_keeper.DataStoreInternals.archive_visibility_delay_secs = $VISIBILITY_SECS |
    .chrono_keeper.DataStoreInternals.shutdown_confirm_timeout_secs = $SHUTDOWN_CONFIRM_SECS |
    .chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK_SECS |
    .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT_SECS |
    .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = $REPORT_SECS" \
    "$CONF_TEMPLATE.wal_shutdown_backup" > "$CONF_TEMPLATE.wal_shutdown_patched" ||
    { say "conf patch failed; template left as it was"; rm -f "$CONF_TEMPLATE.wal_shutdown_backup" "$CONF_TEMPLATE.wal_shutdown_patched"; exit 2; }
mv -f "$CONF_TEMPLATE.wal_shutdown_patched" "$CONF_TEMPLATE"

RUN_DIR=$(mktemp -d /tmp/keeper_wal_shutdown.XXXXXX)
HOLD="$RUN_DIR/hold_sync"
HELD="$RUN_DIR/sync_held"
restore_conf() { mv -f "$CONF_TEMPLATE.wal_shutdown_backup" "$CONF_TEMPLATE"; }
remove_wal() {
    rm -rf "$WAL_DIR"
    rmdir -- "$WORK_DIR/wal" 2> /dev/null || true
}
trap 'cleanup; remove_wal; restore_conf; rm -rf "$RUN_DIR"' EXIT

kill_daemons
sleep 2
rm -f "$MONITOR_DIR"/chrono-*.log
rm -rf "${OUTPUT_DIR:?}/$CHRONICLE"
rm -rf "$WAL_DIR"

export CHRONOLOG_TEST_HOLD_WAL_SYNC="$HOLD"
export CHRONOLOG_TEST_WAL_SYNC_HELD="$HELD"
export LD_PRELOAD="$HOLD_LIB"
say "deploying 1 keeper / 1 recording group with the WAL on and the sync hold preloaded"
"$DEPLOY" -d -w "$WORK_DIR" -k 1 -r 1 --wal > "$RUN_DIR/deploy.out" 2>&1 ||
    { say "deploy failed"; cat "$RUN_DIR/deploy.out"; exit 2; }
sleep 3
if [ "$(pgrep -c -f "$WORK_DIR/bin/chrono-")" -lt 4 ]; then
    say "expected 4 daemons up"; exit 2
fi
client_conf first 5591
client_conf replay 5593

# ------------------------------------------------------- held sync ----
: > "$HOLD"
"$PROBE" --config "$RUN_DIR/first_client_conf.json" --chronicle "$CHRONICLE" --story "$STORY" \
    --write 1 --mode durable --timeout-ms 60000 --attempts 1 > "$RUN_DIR/write_first.out" 2>&1 &
first_pid=$!
deadline=$((SECONDS + 20))
while [ ! -e "$HELD" ] && [ $SECONDS -lt $deadline ]; do
    if ! kill -0 "$first_pid" 2> /dev/null; then
        break
    fi
    sleep 0.1
done
pid=$(keeper_pid)
if [ ! -e "$HELD" ] || [ -z "$pid" ]; then
    bad "sync: the durable write never entered fdatasync"
    cat "$RUN_DIR/write_first.out"
    kill "$first_pid" 2> /dev/null || true
    wait "$first_pid" 2> /dev/null || true
    say "${PASS} passed, ${FAIL} failed"
    exit 1
fi
ok "sync: the durable write is inside fdatasync"

# ---------------------------------------------------------- sigterm ----
kill -TERM "$pid"
# signal() restarts sleep, so the main thread can take the rest of its 10s
# stats sleep before it unregisters and stops taking writes. The held sync
# then keeps wal.stop from finishing, and the recording RPC stays up.
say "waiting 15s for the keeper to stop taking writes"
sleep 15
keeper_proto=$(jq -r '.chrono_keeper.KeeperRecordingService.rpc.protocol_conf' "$KEEPER_CONF")
keeper_ip=$(jq -r '.chrono_keeper.KeeperRecordingService.rpc.service_ip' "$KEEPER_CONF")
keeper_port=$(jq -r '.chrono_keeper.KeeperRecordingService.rpc.service_base_port' "$KEEPER_CONF")
keeper_provider=$(jq -r '.chrono_keeper.KeeperRecordingService.rpc.service_provider_id' "$KEEPER_CONF")
# the probe times the RPC itself, so its own start-up is not counted
"$PROBE" --chronicle "$CHRONICLE" --story "$STORY" --direct-write \
    --keeper-protocol "$keeper_proto" --keeper-ip "$keeper_ip" --keeper-port "$keeper_port" \
    --keeper-provider "$keeper_provider" > "$RUN_DIR/write_later.out" 2>&1 || true
refused_ms=$(sed -n 's/^DIRECT_MS \([0-9][0-9]*\)$/\1/p' "$RUN_DIR/write_later.out")
refused_ms=${refused_ms:-99999}
# "<time> 1 1", the identity the replay must not return
sed -n 's/^DIRECT CL_ERR_DURABILITY_UNAVAILABLE \([0-9][0-9]* [0-9][0-9]* [0-9][0-9]*\)$/\1/p' \
    "$RUN_DIR/write_later.out" > "$RUN_DIR/refused.ids"
if [ -s "$RUN_DIR/refused.ids" ] && [ "$refused_ms" -lt 2000 ]; then
    ok "shutdown: a durable write sent to the keeper got CL_ERR_DURABILITY_UNAVAILABLE in ${refused_ms} ms"
else
    bad "shutdown: the direct durable write did not get CL_ERR_DURABILITY_UNAVAILABLE quickly (${refused_ms} ms)"
    cat "$RUN_DIR/write_later.out" | sed 's/^/    /'
fi

# the sync can finish now; the write that was inside it is answered
rm -f "$HOLD"
wait "$first_pid" || true
identities "$RUN_DIR/write_first.out" > "$RUN_DIR/acked_first"
if [ "$(wc -l < "$RUN_DIR/acked_first")" -eq 1 ]; then
    ok "shutdown: the write that was inside fdatasync was answered CL_SUCCESS"
else
    bad "shutdown: the write that was inside fdatasync was not answered CL_SUCCESS"
    sed -n 's/^WRITE_CODE //p' "$RUN_DIR/write_first.out" | sed 's/^/    /'
fi

# ------------------------------------------------------------- exit ----
say "waiting for keeper $pid to finish shutting down"
deadline=$((SECONDS + SHUTDOWN_CONFIRM_SECS + 30))
while kill -0 "$pid" 2> /dev/null && [ $SECONDS -lt $deadline ]; do
    sleep 1
done
summary=""
pending=""
if [ -f "$WAL_DIR/exit-summary" ]; then
    summary=$(head -n 1 "$WAL_DIR/exit-summary")
    pending=$(sed -n 's/.*pending_events=\([0-9][0-9]*\).*/\1/p' <<< "$summary")
fi
if kill -0 "$pid" 2> /dev/null; then
    bad "exit: keeper $pid still running"
elif [ "$pending" = "0" ] || [ "$pending" = "1" ]; then
    ok "exit: exit-summary $summary"
else
    bad "exit: exit-summary '${summary:-missing}', wanted pending_events of 0 or 1"
fi

# ------------------------------------------------------------ replay ----
say "restarting keeper 1"
"$DEPLOY" --restart-keeper 1 -w "$WORK_DIR" > "$RUN_DIR/restart.out" 2>&1 ||
    { bad "replay: restart failed"; cat "$RUN_DIR/restart.out"; say "${PASS} passed, ${FAIL} failed"; exit 1; }
cp "$RUN_DIR/acked_first" "$RUN_DIR/expect.ids"
found=0
deadline=$((SECONDS + 120))
while [ $SECONDS -lt $deadline ]; do
    "$PROBE" --config "$RUN_DIR/replay_client_conf.json" --chronicle "$CHRONICLE" --story "$STORY" --replay \
        > "$RUN_DIR/replay.out" 2>&1 || true
    R_STATUS=$(sed -n 's/^REPLAY_STATUS \(.*\)$/\1/p' "$RUN_DIR/replay.out")
    grep -E '^[0-9]+ [0-9]+ [0-9]+$' "$RUN_DIR/replay.out" > "$RUN_DIR/replay.ids"
    missing=$(comm -23 "$RUN_DIR/expect.ids" <(sort -u "$RUN_DIR/replay.ids") | wc -l)
    twice=$(awk 'NR == FNR { acked[$1 " " $2] = 1; next } ($1 " " $2) in acked { n[$1 " " $2]++ }
                 END { c = 0; for(k in n) if(n[k] > 1) c++; print c }' \
                "$RUN_DIR/expect.ids" "$RUN_DIR/replay.ids")
    if [ "$R_STATUS" = "CL_SUCCESS" ] && [ "$missing" -eq 0 ] && [ "$twice" -eq 0 ]; then
        found=1
        break
    fi
    sleep 2
done
expected=$(wc -l < "$RUN_DIR/expect.ids")
stored_refused=$(grep -cxFf "$RUN_DIR/refused.ids" "$RUN_DIR/replay.ids" 2> /dev/null)
if [ "$found" -eq 1 ] && [ -s "$RUN_DIR/refused.ids" ] && [ "${stored_refused:-0}" -eq 0 ]; then
    ok "replay: all $expected identities answered CL_SUCCESS came back exactly once, and the refused write did not"
elif [ "$found" -eq 1 ] && [ ! -s "$RUN_DIR/refused.ids" ]; then
    bad "replay: the later write was not refused with CL_ERR_DURABILITY_UNAVAILABLE, so there is no refused identity to look for"
elif [ "$found" -eq 1 ]; then
    bad "replay: the write refused with CL_ERR_DURABILITY_UNAVAILABLE came back ($(cat "$RUN_DIR/refused.ids"))"
else
    bad "replay: status ${R_STATUS:-none}, $missing of $expected missing, $twice returned more than once"
fi

say "${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ]
