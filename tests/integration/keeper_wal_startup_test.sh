#!/bin/bash
# Integration test for what a real keeper does with a WAL it did not itself
# finish writing.
#
# One keeper, WAL on. After the cluster is up the keeper is killed and its
# WAL directory is replaced, twice:
#
#   1. An older segment has a record whose CRC is wrong, with an intact record
#      after it, and a newer segment holds a valid event. The keeper must exit,
#      log that recovery failed, and leave both segment files unchanged.
#   2. The only segment's last group has a record whose CRC is wrong and an
#      intact record after it. The keeper must stay up, leave a
#      "<segment>.wal.tail-<offset>" copy, log that whole records after the
#      bad one were zeroed, and a replay must return the earlier event and not
#      the intact one that followed the bad record.
#
# The segments are written by chronolog-test-wal-fixture. Requires an installed
# tree and the build tree for the fixture and the probe. The script patches
# the installed conf template, deploys 1 keeper / 1 recording group, stops the
# deployment and restores the template.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        ./keeper_wal_startup_test.sh

set -u

WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
BUILD_DIR="${BUILD_DIR:-$HOME/chronolog-build/Debug}"

MONITOR_DIR="$WORK_DIR/monitor"
OUTPUT_DIR="$WORK_DIR/output"
CONF_TEMPLATE="$WORK_DIR/conf/default-chrono-conf.json"
CLIENT_CONF="$WORK_DIR/conf/default-chrono-client-conf.json"
DEPLOY="$WORK_DIR/tools/deploy/deploy_local.sh"
PROBE="$BUILD_DIR/tests/integration/keeper-wal/chronolog-test-durable-probe"
FIXTURE="$BUILD_DIR/tests/integration/keeper-wal/chronolog-test-wal-fixture"
WAL_DIR="$(realpath -m "$WORK_DIR/wal/keeper-1")"
KEEPER_LOG="$MONITOR_DIR/chrono-keeper-1.log"

CHRONICLE=KeeperWalStartupChronicle
STORY=StartupStory
G_CHUNK_SECS=10
G_ACCEPT_SECS=15
REPORT_SECS=1

PASS=0
FAIL=0

say() { echo -e "[keeper_wal_startup_test $(date +%H:%M:%S)] $*"; }
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

# Replace the WAL, then relaunch keeper 1. The log text written after this
# returns is what the following checks read.
replace_wal() { # $1 = fixture mode, $2 = label
    local pid
    pid=$(keeper_pid)
    if [ -n "$pid" ]; then
        kill -9 "$pid"
        for _ in $(seq 1 50); do kill -0 "$pid" 2> /dev/null || break; sleep 0.1; done
    fi
    rm -rf "$WAL_DIR"
    mkdir -p "$WAL_DIR"
    "$FIXTURE" --dir "$WAL_DIR" --chronicle "$CHRONICLE" --story "$STORY" --mode "$1" \
        > "$RUN_DIR/fixture_$2.out" 2>&1 || { say "fixture $1 failed"; cat "$RUN_DIR/fixture_$2.out"; exit 2; }
    # the keeper creates lock when it opens the directory; the segments are what
    # recovery is not allowed to change
    (cd "$WAL_DIR" && find . -maxdepth 1 -name '*.wal' -print0 | sort -z | xargs -0 sha256sum) > "$RUN_DIR/${2}_before.sha"
    LOG_AT=$(wc -c < "$KEEPER_LOG" 2> /dev/null || echo 0)
    say "restarting keeper 1 on a $1 WAL"
    "$DEPLOY" --restart-keeper 1 -w "$WORK_DIR" > "$RUN_DIR/restart_$2.out" 2>&1 ||
        { say "restart failed"; cat "$RUN_DIR/restart_$2.out"; exit 2; }
}

log_since() { tail -c +$((LOG_AT + 1)) "$KEEPER_LOG" 2> /dev/null || true; }

command -v jq > /dev/null || { say "jq not found"; exit 2; }
[ -x "$PROBE" ] || { say "probe not found at $PROBE"; exit 2; }
[ -x "$FIXTURE" ] || { say "fixture not found at $FIXTURE"; exit 2; }
[ -x "$DEPLOY" ] || { say "deploy script not found at $DEPLOY"; exit 2; }

say "patching installed conf template: grapher ${G_CHUNK_SECS}s/${G_ACCEPT_SECS}s windows"
cp "$CONF_TEMPLATE" "$CONF_TEMPLATE.wal_startup_backup"
jq ".chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK_SECS |
    .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT_SECS |
    .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = $REPORT_SECS" \
    "$CONF_TEMPLATE.wal_startup_backup" > "$CONF_TEMPLATE.wal_startup_patched" ||
    { say "conf patch failed; template left as it was"; rm -f "$CONF_TEMPLATE.wal_startup_backup" "$CONF_TEMPLATE.wal_startup_patched"; exit 2; }
mv -f "$CONF_TEMPLATE.wal_startup_patched" "$CONF_TEMPLATE"

RUN_DIR=$(mktemp -d /tmp/keeper_wal_startup.XXXXXX)
restore_conf() { mv -f "$CONF_TEMPLATE.wal_startup_backup" "$CONF_TEMPLATE"; }
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

say "deploying 1 keeper / 1 recording group with the WAL on"
"$DEPLOY" -d -w "$WORK_DIR" -k 1 -r 1 --wal > "$RUN_DIR/deploy.out" 2>&1 ||
    { say "deploy failed"; cat "$RUN_DIR/deploy.out"; exit 2; }
sleep 3
if [ "$(pgrep -c -f "$WORK_DIR/bin/chrono-")" -lt 4 ]; then
    say "expected 4 daemons up"; exit 2
fi
jq ".chrono_client.ClientQueryService.rpc.service_base_port = 5601" \
    "$CLIENT_CONF" > "$RUN_DIR/replay_client_conf.json" || { say "client conf patch failed"; exit 2; }

# -------------------------------------------------------- older bad ----
replace_wal older-bad older
exited=0
for _ in $(seq 1 40); do
    [ -z "$(keeper_pid)" ] && exited=1 && break
    sleep 0.5
done
(cd "$WAL_DIR" && find . -maxdepth 1 -name '*.wal' -print0 | sort -z | xargs -0 sha256sum) > "$RUN_DIR/older_after.sha"
if [ "$exited" -eq 1 ] && log_since | grep -q "WAL recovery failed"; then
    ok "older segment: the keeper exited and logged that WAL recovery failed"
elif [ "$exited" -eq 1 ]; then
    bad "older segment: the keeper exited without logging 'WAL recovery failed'"
    log_since | tail -20 | sed 's/^/    /'
else
    bad "older segment: the keeper was still running 20s after restart"
fi
if cmp -s "$RUN_DIR/older_before.sha" "$RUN_DIR/older_after.sha"; then
    ok "older segment: the WAL files were not changed"
else
    bad "older segment: the WAL files changed"
    diff -u "$RUN_DIR/older_before.sha" "$RUN_DIR/older_after.sha" | sed 's/^/    /'
fi

# -------------------------------------------------------- last group ----
rm -rf "${OUTPUT_DIR:?}/$CHRONICLE"
replace_wal last-group last
stayed=0
for _ in $(seq 1 20); do
    [ -n "$(keeper_pid)" ] && stayed=1 && break
    sleep 0.25
done
sleep 2
if [ "$stayed" -eq 1 ] && [ -n "$(keeper_pid)" ]; then
    ok "last group: the keeper stayed up"
else
    bad "last group: the keeper did not stay up"
    log_since | tail -20 | sed 's/^/    /'
fi
tails=$(find "$WAL_DIR" -maxdepth 1 -name '*.wal.tail-*' | wc -l)
if [ "$tails" -ge 1 ]; then
    ok "last group: recovery left a torn-tail copy ($(find "$WAL_DIR" -maxdepth 1 -name '*.wal.tail-*' -printf '%f\n' | paste -sd ' ' -))"
else
    bad "last group: no <segment>.wal.tail-<offset> file in $WAL_DIR"
fi
if log_since | grep -q "whole record"; then
    ok "last group: recovery logged that whole records after the bad one were zeroed"
else
    bad "last group: the keeper log does not mention the whole records after the bad one"
    log_since | tail -20 | sed 's/^/    /'
fi

kept=$(sed -n 's/^KEPT //p' "$RUN_DIR/fixture_last.out")
dropped=$(sed -n 's/^DROPPED //p' "$RUN_DIR/fixture_last.out")
found=0
R_STATUS=""
deadline=$((SECONDS + 90))
while [ $SECONDS -lt $deadline ]; do
    "$PROBE" --config "$RUN_DIR/replay_client_conf.json" --chronicle "$CHRONICLE" --story "$STORY" --replay \
        > "$RUN_DIR/replay.out" 2>&1 || true
    R_STATUS=$(sed -n 's/^REPLAY_STATUS \(.*\)$/\1/p' "$RUN_DIR/replay.out")
    grep -E '^[0-9]+ [0-9]+ [0-9]+$' "$RUN_DIR/replay.out" > "$RUN_DIR/replay.ids"
    if [ "$R_STATUS" = "CL_SUCCESS" ] && grep -qxF "$kept" "$RUN_DIR/replay.ids" &&
        ! grep -qxF "$dropped" "$RUN_DIR/replay.ids"; then
        found=1
        break
    fi
    sleep 2
done
if [ "$found" -eq 1 ]; then
    ok "last group: replay returned the event from before the bad record and not the intact one after it"
else
    bad "last group: replay status ${R_STATUS:-none}; wanted '$kept' and not '$dropped'"
    cat "$RUN_DIR/replay.ids" | sed 's/^/    /'
fi

say "${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ]
