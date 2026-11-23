#!/bin/bash
# Integration test for a keeper sent one SIGTERM while its last write is not
# yet confirmed by the grapher.
#
# One keeper, WAL off, shutdown_ms cut to 3 s, shutdown_confirm_timeout_secs
# 40 s. One accepted write is sent and the keeper is sent SIGTERM right after
# it. The keeper waits for the grapher to confirm the chunk holding the
# write, which takes longer than 3 s: the grapher's merged chunks here are
# 10 s with a 10 s acceptance window. So the first stop attempt runs out
# while that wait is still going.
#
#   1. The keeper's first stop attempt ran out with a step still running
#      ("stop attempt 1 ran out").
#   2. The keeper exits by itself, with no second SIGTERM, once the grapher
#      has confirmed the chunk.
#   3. The grapher confirmed inside the 40 s cap: the keeper did not log that
#      it gave up on the confirmation.
#
# Requires an installed tree and the build tree for the probe. The script
# patches the installed conf template, deploys 1 keeper / 1 recording group,
# stops the deployment and restores the template.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        ./keeper_sigterm_stop_test.sh

set -u

WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
BUILD_DIR="${BUILD_DIR:-$HOME/chronolog-build/Debug}"
MONITOR_DIR="$WORK_DIR/monitor"
OUTPUT_DIR="$WORK_DIR/output"
CONF_TEMPLATE="$WORK_DIR/conf/default-chrono-conf.json"
CLIENT_CONF="$WORK_DIR/conf/default-chrono-client-conf.json"
DEPLOY="$WORK_DIR/tools/deploy/deploy_local.sh"
PROBE="$BUILD_DIR/tests/integration/keeper-wal/chronolog-test-durable-probe"

CHRONICLE=KeeperSigtermStopChronicle
SHUTDOWN_MS=3000
CONFIRM_SECS=40
K_CHUNK_SECS=2
K_ACCEPT_SECS=3
G_CHUNK_SECS=10
G_ACCEPT_SECS=10
EXIT_WAIT_SECS=90
GAVE_UP='The grapher did not confirm every chunk written within'

PASS=0
FAIL=0

say() { echo -e "[keeper_sigterm_stop_test $(date +%H:%M:%S)] $*"; }
ok() { say "PASS: $*"; PASS=$((PASS + 1)); }
bad() { say "FAIL: $*"; FAIL=$((FAIL + 1)); }

kill_daemons() {
    for bin in chrono-visor chrono-keeper chrono-grapher chrono-player; do
        pkill -9 -f "$WORK_DIR/bin/$bin" 2> /dev/null
    done
}

cleanup() {
    "$DEPLOY" -s -w "$WORK_DIR" > /dev/null 2>&1
    kill_daemons
}

keeper_pid() { pgrep -f "$WORK_DIR/bin/chrono-keeper --config" | head -1; }

command -v jq > /dev/null || { say "jq not found"; exit 2; }
[ -x "$PROBE" ] || { say "probe not found at $PROBE"; exit 2; }
[ -x "$DEPLOY" ] || { say "deploy script not found at $DEPLOY"; exit 2; }

say "patching installed conf template: keeper shutdown_ms ${SHUTDOWN_MS}, confirm cap ${CONFIRM_SECS}s, grapher ${G_CHUNK_SECS}s/${G_ACCEPT_SECS}s chunks"
cp "$CONF_TEMPLATE" "$CONF_TEMPLATE.sigterm_stop_backup"
jq ".chrono_keeper.RpcPolicy.shutdown_ms = $SHUTDOWN_MS |
    .chrono_keeper.DataStoreInternals.shutdown_confirm_timeout_secs = $CONFIRM_SECS |
    .chrono_keeper.DataStoreInternals.story_chunk_duration_secs = $K_CHUNK_SECS |
    .chrono_keeper.DataStoreInternals.acceptance_window_secs = $K_ACCEPT_SECS |
    .chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK_SECS |
    .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT_SECS" \
    "$CONF_TEMPLATE.sigterm_stop_backup" > "$CONF_TEMPLATE.sigterm_stop_patched" ||
    { say "conf patch failed; template left as it was"; rm -f "$CONF_TEMPLATE.sigterm_stop_backup" "$CONF_TEMPLATE.sigterm_stop_patched"; exit 2; }
mv -f "$CONF_TEMPLATE.sigterm_stop_patched" "$CONF_TEMPLATE"

RUN_DIR=$(mktemp -d /tmp/keeper_sigterm_stop.XXXXXX)
restore_conf() { mv -f "$CONF_TEMPLATE.sigterm_stop_backup" "$CONF_TEMPLATE"; }
trap 'cleanup; restore_conf; rm -rf "$RUN_DIR"' EXIT

# Deploys, writes one accepted event to STORY, sends one SIGTERM to the
# keeper and waits for it to exit. Sets pid and elapsed, and copies the
# keeper log to $RUN_DIR/<phase>.keeper.log.
run_phase() {
    local phase=$1 story=$2
    kill_daemons
    sleep 2
    rm -f "$MONITOR_DIR"/chrono-*.log
    rm -rf "${OUTPUT_DIR:?}/$CHRONICLE"

    say "$phase: deploying 1 keeper / 1 recording group"
    "$DEPLOY" -d -w "$WORK_DIR" -k 1 -r 1 > "$RUN_DIR/$phase.deploy.out" 2>&1 ||
        { say "deploy failed"; cat "$RUN_DIR/$phase.deploy.out"; exit 2; }
    sleep 3
    pid=$(keeper_pid)
    [ -n "$pid" ] || { say "no keeper running"; exit 2; }
    jq ".chrono_client.ClientQueryService.rpc.service_base_port = 5595" "$CLIENT_CONF" > "$RUN_DIR/client_conf.json" ||
        { say "client conf patch failed"; exit 2; }

    "$PROBE" --config "$RUN_DIR/client_conf.json" --chronicle "$CHRONICLE" --story "$story" \
        --write 1 --mode accepted > "$RUN_DIR/$phase.write.out" 2>&1 ||
        { say "write failed"; cat "$RUN_DIR/$phase.write.out"; exit 2; }
    grep -q '^WRITE_CODE CL_SUCCESS 1' "$RUN_DIR/$phase.write.out" ||
        { say "the write was not accepted"; cat "$RUN_DIR/$phase.write.out"; exit 2; }

    kill -TERM "$pid"
    local started=$SECONDS deadline=$((SECONDS + EXIT_WAIT_SECS))
    while kill -0 "$pid" 2> /dev/null && [ $SECONDS -lt $deadline ]; do
        sleep 1
    done
    elapsed=$((SECONDS - started))
    cat "$MONITOR_DIR"/chrono-keeper*.log > "$RUN_DIR/$phase.keeper.log" 2> /dev/null
}

# ------------------------------------------------------- slow grapher ----
run_phase slow SigtermStopStory
if grep -q 'stop attempt 1 ran out' "$RUN_DIR/slow.keeper.log"; then
    ok "slow: the first ${SHUTDOWN_MS} ms stop attempt ran out while the write was unconfirmed"
else
    bad "slow: no 'stop attempt 1 ran out' in the keeper log, so the wait never outlasted one attempt"
fi
if kill -0 "$pid" 2> /dev/null; then
    bad "slow: keeper $pid still running ${EXIT_WAIT_SECS}s after its only SIGTERM"
else
    ok "slow: keeper $pid stopped by itself ${elapsed}s after its only SIGTERM"
fi
if grep -q "$GAVE_UP" "$RUN_DIR/slow.keeper.log"; then
    bad "slow: the keeper gave up on the grapher's confirmation within ${CONFIRM_SECS}s"
else
    ok "slow: the grapher confirmed the chunk inside the ${CONFIRM_SECS}s cap"
fi
say "${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ]
