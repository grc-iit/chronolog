#!/bin/bash
# Integration test for durable writes through the keeper WAL: a keeper killed
# right after it acknowledged durable writes must, once restarted, ship every
# one of them to the archive, while accepted writes made just before the same
# kill are lost.
#
# One keeper, WAL on (deploy_local.sh --wal). A client writes EVENTS accepted
# events to one story and then EVENTS durable events to another, recording the
# identity (time, client id, index) of every event the keeper acknowledged.
# The keeper is killed with SIGKILL within KILL_WITHIN_SECS of the last durable
# ack. The keeper conf is pinned to K_CHUNK_SECS chunks and a K_ACCEPT_SECS
# acceptance window: a keeper chunk ships once now >= chunk end + window, and
# the ReleaseStory after the accepted writes finalizes that story at release +
# window, so as long as the kill comes less than K_ACCEPT_SECS after the first
# accepted event, nothing the keeper held in memory has reached the grapher.
# It is restarted with deploy_local.sh --restart-keeper 1, which reuses its conf
# and WAL directory.
#
#   1. The kill came within KILL_WITHIN_SECS of the last durable ack and less
#      than K_ACCEPT_SECS after the first accepted event, and the killed keeper
#      left no exit-summary.
#   2. The restarted keeper recovers the WAL and ships the durable events: the
#      archive holds at least EVENTS events of the durable story, and the
#      segment that held their records stays until it does.
#   3. A replay of the durable story returns every acknowledged identity
#      exactly once (no identity missing, no (time, client id) twice).
#   4. Negative control: a replay of the accepted story, answered CL_SUCCESS,
#      misses at least one acknowledged event.
#   5. Once the grapher confirmed the recovered chunks, the segment that held
#      the durable records is deleted and the WAL directory holds at most two
#      *.wal files: the active segment and the spare.
#   6. deploy_local.sh --stop reads the keeper's exit-summary with no pending
#      durable events.
#
# The accepted and durable writes share one deployment and one kill, so the
# negative control runs under the same conditions as the durable run.
#
# Requires an installed tree (deploy_local.sh layout) and the build tree for
# the probe. The script patches the *installed* conf template (never the source
# tree), pins the keeper's chunk and acceptance times, shortens the grapher
# windows so the archive is written quickly, deploys 1 keeper / 1 recording
# group, runs the scenario, stops the deployment and restores the template.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        ./keeper_wal_crash_test.sh

set -u

WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
BUILD_DIR="${BUILD_DIR:-$HOME/chronolog-build/Debug}"
REPO_ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../")"
H5DUMP="${H5DUMP:-$REPO_ROOT/.spack-env/view/bin/h5dump}"

MONITOR_DIR="$WORK_DIR/monitor"
OUTPUT_DIR="$WORK_DIR/output"
CONF_TEMPLATE="$WORK_DIR/conf/default-chrono-conf.json"
CLIENT_CONF="$WORK_DIR/conf/default-chrono-client-conf.json"
DEPLOY="$WORK_DIR/tools/deploy/deploy_local.sh"
PROBE="$BUILD_DIR/tests/integration/keeper-wal/chronolog-test-durable-probe"
# deploy_local.sh puts keeper n's WAL in work_dir/wal/keeper-<n>
WAL_DIR="$(realpath -m "$WORK_DIR/wal/keeper-1")"

CHRONICLE=KeeperWalCrashChronicle
DURABLE_STORY=DurableStory
ACCEPTED_STORY=AcceptedStory
EVENTS=200
KILL_WITHIN_SECS=2

# keeper chunks and acceptance window pinned so the kill is known to come
# before any accepted chunk ships (see above)
K_CHUNK_SECS=10
K_ACCEPT_SECS=15
# grapher windows patched small so the archive is written soon after the restart
G_CHUNK_SECS=10
G_ACCEPT_SECS=15
REPORT_SECS=1
VISIBILITY_SECS=3
ARCHIVE_DEADLINE=240
WAL_DEADLINE=120

PASS=0
FAIL=0

say() { echo -e "[keeper_wal_crash_test $(date +%H:%M:%S)] $*"; }
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

now_ns() { date +%s%N; }

keeper_pid() { pgrep -f "$WORK_DIR/bin/chrono-keeper --config" | head -1; }

wal_segments() { find "$WAL_DIR" -maxdepth 1 -name '*.wal' 2> /dev/null | wc -l; }
oldest_segment() { find "$WAL_DIR" -maxdepth 1 -name '*.wal' -printf '%f\n' 2> /dev/null | sort | head -1; }

h5_story_events() { # $1 = story; total events across its archive files
    local total=0 f n
    for f in "$OUTPUT_DIR/$CHRONICLE/$1"/*.h5; do
        [ -f "$f" ] || continue
        n=$("$H5DUMP" -H -d /story_chunks/data.vlen_bytes "$f" 2> /dev/null |
            sed -n 's/.*DATASPACE *SIMPLE *{ *( *\([0-9]*\) *).*/\1/p' | head -1)
        total=$((total + ${n:-0}))
    done
    echo "$total"
}

identities() { grep -E '^[0-9]+ [0-9]+ [0-9]+$' "$1" | sort -u; } # "<time> <client id> <index>" lines

# write <story> <mode>: writes EVENTS events; the acknowledged identities go to $RUN_DIR/acked_<mode>
write_story() {
    local out="$RUN_DIR/write_$2.out"
    "$PROBE" --config "$WRITER_CLIENT_CONF" --chronicle "$CHRONICLE" --story "$1" --write "$EVENTS" --mode "$2" \
        > "$out" 2>&1
    identities "$out" > "$RUN_DIR/acked_$2"
    say "$2 writes to $1: $(sed -n 's/^WRITE_ACKED //p' "$out") acked/sent; codes: $(sed -n 's/^WRITE_CODE //p' "$out" | paste -sd ',' -)"
    [ "$(wc -l < "$RUN_DIR/acked_$2")" -eq "$EVENTS" ]
}

# replay <story> <label>: sets R_STATUS; the returned identities go to $RUN_DIR/replay_<label>.ids
replay_story() {
    local out="$RUN_DIR/replay_$2.out"
    "$PROBE" --config "$RUN_DIR/$2_client_conf.json" --chronicle "$CHRONICLE" --story "$1" --replay > "$out" 2>&1
    R_STATUS=$(sed -n 's/^REPLAY_STATUS \(.*\)$/\1/p' "$out")
    grep -E '^[0-9]+ [0-9]+ [0-9]+$' "$out" > "$RUN_DIR/replay_$2.ids"
    say "replay of $1: status ${R_STATUS:-?}, $(wc -l < "$RUN_DIR/replay_$2.ids") event(s)"
}

# ---------------------------------------------------------------- setup ----
command -v jq > /dev/null || { say "jq not found"; exit 2; }
[ -x "$PROBE" ] || { say "probe not found at $PROBE"; exit 2; }
[ -x "$H5DUMP" ] || { say "h5dump not found at $H5DUMP"; exit 2; }
[ -x "$DEPLOY" ] || { say "deploy script not found at $DEPLOY"; exit 2; }

say "patching installed conf template: keeper ${K_CHUNK_SECS}s/${K_ACCEPT_SECS}s chunks, grapher ${G_CHUNK_SECS}s/${G_ACCEPT_SECS}s windows, report ${REPORT_SECS}s, keeper visibility ${VISIBILITY_SECS}s"
# jq writes to a side file that replaces the template only on success, so a failed
# patch leaves the template intact (the restore trap below is not set up yet)
cp "$CONF_TEMPLATE" "$CONF_TEMPLATE.wal_crash_backup"
jq ".chrono_keeper.DataStoreInternals.story_chunk_duration_secs = $K_CHUNK_SECS |
    .chrono_keeper.DataStoreInternals.acceptance_window_secs = $K_ACCEPT_SECS |
    .chrono_keeper.DataStoreInternals.archive_visibility_delay_secs = $VISIBILITY_SECS |
    .chrono_keeper.DataStoreInternals.shutdown_confirm_timeout_secs = 30 |
    .chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK_SECS |
    .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT_SECS |
    .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = $REPORT_SECS" \
    "$CONF_TEMPLATE.wal_crash_backup" > "$CONF_TEMPLATE.wal_crash_patched" ||
    { say "conf patch failed; template left as it was"; rm -f "$CONF_TEMPLATE.wal_crash_backup" "$CONF_TEMPLATE.wal_crash_patched"; exit 2; }
mv -f "$CONF_TEMPLATE.wal_crash_patched" "$CONF_TEMPLATE"

RUN_DIR=$(mktemp -d /tmp/keeper_wal_crash.XXXXXX)
restore_conf() { mv -f "$CONF_TEMPLATE.wal_crash_backup" "$CONF_TEMPLATE"; }
trap 'cleanup; restore_conf; rm -rf "$RUN_DIR"' EXIT

kill_daemons
sleep 2
rm -f "$MONITOR_DIR"/chrono-*.log
rm -rf "${OUTPUT_DIR:?}/$CHRONICLE" "$OUTPUT_DIR"/"$CHRONICLE".*.h5
# a WAL left by an earlier run would be recovered into this one
rm -rf "$WAL_DIR"

say "deploying 1 keeper / 1 recording group with the WAL on in $WAL_DIR"
"$DEPLOY" -d -w "$WORK_DIR" -k 1 -r 1 --wal > "$RUN_DIR/deploy.out" 2>&1 || { say "deploy failed (see output below)"; cat "$RUN_DIR/deploy.out"; exit 2; }
sleep 3
if [ "$(pgrep -c -f "$WORK_DIR/bin/chrono-")" -lt 4 ]; then
    say "expected 4 daemons up"; exit 2
fi
# the writer and each replay probe bind their query services on ports of their
# own, so a probe never waits for the previous one's port to be released
for client in writer:${WRITER_QUERY_PORT:-5564} durable:${PROBE_QUERY_PORT:-5563} accepted:${ACCEPTED_QUERY_PORT:-5565}; do
    jq ".chrono_client.ClientQueryService.rpc.service_base_port = ${client#*:}" \
        "$CLIENT_CONF" > "$RUN_DIR/${client%%:*}_client_conf.json" || { say "${client%%:*} client conf patch failed"; exit 2; }
done
WRITER_CLIENT_CONF="$RUN_DIR/writer_client_conf.json"

# ------------------------------------------------- write, then kill ----
write_story "$ACCEPTED_STORY" accepted || { bad "accepted writes: not every event was acknowledged"; exit 1; }
write_story "$DURABLE_STORY" durable || { bad "durable writes: not every event was acknowledged"; exit 1; }
last_ack_ns=$(sed -n 's/^LAST_ACK_NS //p' "$RUN_DIR/write_durable.out")
first_accepted_ns=$(cut -d' ' -f1 "$RUN_DIR/acked_accepted" | sort -n | head -1)

pid=$(keeper_pid)
[ -n "$pid" ] || { bad "kill: no keeper running after the writes"; exit 1; }
kill -9 "$pid"
kill_ns=$(now_ns)
gap_ms=$(((kill_ns - last_ack_ns) / 1000000))
span_ms=$(((kill_ns - first_accepted_ns) / 1000000))
for _ in $(seq 1 50); do kill -0 "$pid" 2> /dev/null || break; sleep 0.1; done
if kill -0 "$pid" 2> /dev/null; then
    bad "kill: keeper $pid still running after SIGKILL"; exit 1
elif [ "$gap_ms" -lt $((KILL_WITHIN_SECS * 1000)) ]; then
    ok "kill: keeper $pid killed ${gap_ms} ms after the last durable ack"
else
    bad "kill: keeper killed ${gap_ms} ms after the last durable ack, over ${KILL_WITHIN_SECS}s"
fi
# a later kill could come after the first accepted chunk shipped, and the
# negative control would then not show what it claims
if [ "$span_ms" -lt $((K_ACCEPT_SECS * 1000)) ]; then
    ok "kill: ${span_ms} ms after the first accepted event, under the ${K_ACCEPT_SECS}s acceptance window"
else
    bad "kill: ${span_ms} ms after the first accepted event, not under the ${K_ACCEPT_SECS}s acceptance window; accepted chunks may have shipped"
fi
# the oldest segment holds the durable records; the newer one is the spare
held_segment=$(oldest_segment)
if [ -e "$WAL_DIR/exit-summary" ]; then
    bad "kill: the killed keeper left $WAL_DIR/exit-summary, as only a clean exit should"
else
    ok "kill: no exit-summary in $WAL_DIR after the kill; $(wal_segments) *.wal file(s) in it, records in ${held_segment:-none}"
fi

# ---------------------------------------------------------- restart ----
say "restarting keeper 1"
"$DEPLOY" --restart-keeper 1 -w "$WORK_DIR" > "$RUN_DIR/restart.out" 2>&1 ||
    { bad "restart: deploy_local.sh --restart-keeper 1 failed"; cat "$RUN_DIR/restart.out"; exit 1; }
restart_ns=$(now_ns)

say "waiting up to ${ARCHIVE_DEADLINE}s for $EVENTS events of $DURABLE_STORY in the archive"
# The keeper may delete the held segment only after the grapher confirmed the
# recovered chunks, which it does after writing them. The segment is looked at
# before the archive is counted, so a segment gone while the count is short was
# deleted before the archive held the events.
held_present() { [ -z "$held_segment" ] || [ -e "$WAL_DIR/$held_segment" ]; }
early_release=0
deadline=$((SECONDS + ARCHIVE_DEADLINE))
held_present || early_release=1
archived=$(h5_story_events "$DURABLE_STORY")
while [ "$archived" -lt "$EVENTS" ] && [ $early_release -eq 0 ] && [ $SECONDS -lt $deadline ]; do
    [ -n "$(keeper_pid)" ] || break
    sleep 5
    held_present || early_release=1
    archived=$(h5_story_events "$DURABLE_STORY")
done
if [ $early_release -eq 1 ] && [ "$archived" -lt "$EVENTS" ]; then
    bad "recovery: $held_segment deleted while only $archived of $EVENTS durable event(s) were in the archive"
elif [ -z "$(keeper_pid)" ]; then
    bad "recovery: the restarted keeper exited; see $MONITOR_DIR/chrono-keeper-1.launch.log"; exit 1
elif [ "$archived" -ge "$EVENTS" ]; then
    ok "recovery: $archived event(s) of $DURABLE_STORY in the archive $(((($(now_ns) - restart_ns) / 1000000000)))s after the restart"
else
    bad "recovery: only $archived of $EVENTS durable event(s) in the archive after ${ARCHIVE_DEADLINE}s"
fi

# The restarted keeper writes on in the old spare and prepares a new one, so
# the WAL holds three segments until the grapher's confirmation of the
# recovered chunks releases their records and the old segment is deleted.
say "waiting up to ${WAL_DEADLINE}s for $held_segment to be deleted and the WAL to drop to at most 2 segments"
deadline=$((SECONDS + WAL_DEADLINE))
while { [ -e "$WAL_DIR/$held_segment" ] || [ "$(wal_segments)" -gt 2 ]; } && [ $SECONDS -lt $deadline ]; do
    sleep 2
done
segments=$(wal_segments)
if [ -z "$held_segment" ]; then
    bad "WAL: no *.wal file in $WAL_DIR after the kill"
elif [ -e "$WAL_DIR/$held_segment" ]; then
    bad "WAL: $held_segment, which held the durable records, still in $WAL_DIR after ${WAL_DEADLINE}s"
elif [ "$segments" -le 2 ]; then
    ok "WAL: $held_segment deleted after confirmation; $segments *.wal file(s) left in $WAL_DIR"
else
    bad "WAL: still $segments *.wal file(s) in $WAL_DIR after ${WAL_DEADLINE}s: $(cd "$WAL_DIR" && ls ./*.wal | paste -sd ' ' -)"
fi

# ----------------------------------------------------------- replay ----
replay_story "$DURABLE_STORY" durable
missing=$(comm -23 "$RUN_DIR/acked_durable" <(sort -u "$RUN_DIR/replay_durable.ids") | wc -l)
# an identity counted twice, or a second event at the same (time, client id)
# under another index, would be a second copy of an acknowledged event
twice=$(awk 'NR == FNR { acked[$1 " " $2] = 1; next } ($1 " " $2) in acked { n[$1 " " $2]++ }
             END { c = 0; for(k in n) if(n[k] > 1) c++; print c }' \
            "$RUN_DIR/acked_durable" "$RUN_DIR/replay_durable.ids")
unexpected=$(comm -13 "$RUN_DIR/acked_durable" <(sort -u "$RUN_DIR/replay_durable.ids") | wc -l)
if [ "$R_STATUS" != "CL_SUCCESS" ]; then
    bad "durable replay: status $R_STATUS"
elif [ "$missing" -eq 0 ] && [ "$twice" -eq 0 ]; then
    ok "durable replay: every one of the $EVENTS acknowledged identities appears exactly once ($unexpected unacknowledged)"
else
    bad "durable replay: $missing acknowledged identities missing, $twice returned more than once"
fi

# a failed replay returns no identities, which must not count as lost events;
# an empty but complete replay returns CL_SUCCESS
replay_story "$ACCEPTED_STORY" accepted
lost=$(comm -23 "$RUN_DIR/acked_accepted" <(sort -u "$RUN_DIR/replay_accepted.ids") | wc -l)
if [ "$R_STATUS" != "CL_SUCCESS" ]; then
    bad "negative control: accepted replay status ${R_STATUS:-none}"
    grep -Ev '^[0-9]+ [0-9]+ [0-9]+$' "$RUN_DIR/replay_accepted.out" | tail -5 | sed 's/^/    /'
elif [ "$lost" -ge 1 ]; then
    ok "negative control: $lost of $EVENTS acknowledged accepted events lost in the kill"
else
    bad "negative control: every acknowledged accepted event survived the kill; the kill did not test the WAL"
fi

# ------------------------------------------------------------- stop ----
say "stopping the deployment"
if "$DEPLOY" -s -w "$WORK_DIR" > "$RUN_DIR/stop.out" 2>&1; then
    ok "stop: exit-summary $(tr -d '\n' < "$WAL_DIR/exit-summary" 2> /dev/null) in $WAL_DIR"
else
    bad "stop: deploy_local.sh --stop reported a missing exit-summary or unconfirmed durable events"
    grep -A3 'PENDING_EVENTS' "$RUN_DIR/stop.out"
fi
grep -h "WAL replay from" "$MONITOR_DIR"/chrono-keeper-1.log 2> /dev/null | tail -1 | sed 's/^/    /'

# ------------------------------------------------------------------ report ----
say "${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ]
