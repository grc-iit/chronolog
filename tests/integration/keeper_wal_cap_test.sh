#!/bin/bash
# Integration test for durable_cap_mb with the grapher stopped.
#
# One keeper, WAL on, durable_cap_mb pinned to 1. A client acquires the story
# and holds it, which is what lets the visor finish starting the story on the
# grapher. The grapher is then stopped with SIGSTOP, so a later acquire does
# not call it again and nothing it has not confirmed can leave the cap. The
# writer sends a burst of 64 KiB events that stays under a chunk's seal time,
# so every one of them is acknowledged. Once that chunk has sealed, the next
# durable write gets CL_ERR_DURABILITY_FULL. The grapher is then continued.
# Durable writes are admitted again, and a replay returns every acknowledged
# identity exactly once and none of the refused writes.
#
#   1. Every event in the burst is acknowledged while the grapher is stopped.
#   2. The archive is still empty, so the cap is not being released underneath
#      the test.
#   3. The next durable write is CL_ERR_DURABILITY_FULL.
#   4. After the grapher runs, a durable write is acknowledged again.
#   5. A replay returns every acknowledged identity exactly once, and no event
#      with the client id and index of a write refused with
#      CL_ERR_DURABILITY_FULL, so a refused write stored nothing.
#
# Requires an installed tree and the build tree for the probe. The script
# patches the installed conf template, deploys 1 keeper / 1 recording group,
# stops the deployment and restores the template. The grapher is continued
# before the deployment is stopped.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        ./keeper_wal_cap_test.sh

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
WAL_DIR="$(realpath -m "$WORK_DIR/wal/keeper-1")"

CHRONICLE=KeeperWalCapChronicle
STORY=CapStory
BURST=20
PAYLOAD_BYTES=65536
# the chunk must still be open for the whole burst, then seal before the probe
K_CHUNK_SECS=2
K_ACCEPT_SECS=3
G_CHUNK_SECS=5
G_ACCEPT_SECS=5
REPORT_SECS=1
VISIBILITY_SECS=1
SEAL_WAIT_SECS=10
RESUME_DEADLINE=120

PASS=0
FAIL=0
GRAPHER_PID=""
HOLDER_PID=""

say() { echo -e "[keeper_wal_cap_test $(date +%H:%M:%S)] $*"; }
ok() { say "PASS: $*"; PASS=$((PASS + 1)); }
bad() { say "FAIL: $*"; FAIL=$((FAIL + 1)); }

kill_daemons() {
    for bin in chrono-visor chrono-keeper chrono-grapher chrono-player; do
        pkill -9 -f "$WORK_DIR/bin/$bin" 2> /dev/null
    done
}

resume_grapher() {
    if [ -n "$GRAPHER_PID" ]; then
        kill -CONT "$GRAPHER_PID" 2> /dev/null || true
    fi
}

release_holder() {
    if [ -n "${RUN_DIR:-}" ]; then
        touch "$RUN_DIR/holder_release" 2> /dev/null || true
    fi
    if [ -n "$HOLDER_PID" ]; then
        wait "$HOLDER_PID" 2> /dev/null || true
        HOLDER_PID=""
    fi
}

cleanup() {
    resume_grapher
    release_holder
    "$DEPLOY" -s -w "$WORK_DIR" > /dev/null 2>&1
    kill_daemons
}

h5_story_events() {
    local total=0 f n
    for f in "$OUTPUT_DIR/$CHRONICLE/$STORY"/*.h5; do
        [ -f "$f" ] || continue
        n=$("$H5DUMP" -H -d /story_chunks/data.vlen_bytes "$f" 2> /dev/null |
            sed -n 's/.*DATASPACE *SIMPLE *{ *( *\([0-9]*\) *).*/\1/p' | head -1)
        total=$((total + ${n:-0}))
    done
    echo "$total"
}

identities() { grep -E '^[0-9]+ [0-9]+ [0-9]+$' "$1" | sort -u; }

client_conf() {
    jq ".chrono_client.ClientQueryService.rpc.service_base_port = $2" \
        "$CLIENT_CONF" > "$RUN_DIR/$1_client_conf.json" || { say "$1 client conf patch failed"; exit 2; }
}

# write <label> <n>: sets W_ACKED and W_CODES; identities go to acked_<label>,
# and the "<client id> <index>" of each CL_ERR_DURABILITY_FULL refusal is added
# to refused_full
write_story() {
    local out="$RUN_DIR/write_$1.out"
    "$PROBE" --config "$RUN_DIR/$1_client_conf.json" --chronicle "$CHRONICLE" --story "$STORY" \
        --write "$2" --mode durable --bytes "$PAYLOAD_BYTES" --attempts 1 --timeout-ms 10000 \
        > "$out" 2>&1
    identities "$out" > "$RUN_DIR/acked_$1"
    sed -n 's/^NOT_ACKED CL_ERR_DURABILITY_FULL \([0-9][0-9]*\) \([0-9][0-9]*\)$/\1 \2/p' "$out" \
        >> "$RUN_DIR/refused_full"
    W_ACKED=$(wc -l < "$RUN_DIR/acked_$1")
    W_CODES=$(sed -n 's/^WRITE_CODE //p' "$out" | paste -sd ',' -)
    say "durable write ($1): $W_ACKED acked of $2; codes: ${W_CODES:-none}"
}

code_count() { tr ',' '\n' <<< "${W_CODES:-}" | sed -n "s/^$1 //p"; }

replay_story() {
    local out="$RUN_DIR/replay.out"
    "$PROBE" --config "$RUN_DIR/replay_client_conf.json" --chronicle "$CHRONICLE" --story "$STORY" --replay \
        > "$out" 2>&1
    R_STATUS=$(sed -n 's/^REPLAY_STATUS \(.*\)$/\1/p' "$out")
    grep -E '^[0-9]+ [0-9]+ [0-9]+$' "$out" > "$RUN_DIR/replay.ids"
    say "replay: status ${R_STATUS:-?}, $(wc -l < "$RUN_DIR/replay.ids") event(s)"
}

command -v jq > /dev/null || { say "jq not found"; exit 2; }
[ -x "$PROBE" ] || { say "probe not found at $PROBE"; exit 2; }
[ -x "$H5DUMP" ] || { say "h5dump not found at $H5DUMP"; exit 2; }
[ -x "$DEPLOY" ] || { say "deploy script not found at $DEPLOY"; exit 2; }

say "patching installed conf template: durable_cap_mb 1, keeper ${K_CHUNK_SECS}s/${K_ACCEPT_SECS}s chunks"
cp "$CONF_TEMPLATE" "$CONF_TEMPLATE.wal_cap_backup"
jq ".chrono_keeper.WAL.durable_cap_mb = 1 |
    .chrono_keeper.DataStoreInternals.story_chunk_duration_secs = $K_CHUNK_SECS |
    .chrono_keeper.DataStoreInternals.acceptance_window_secs = $K_ACCEPT_SECS |
    .chrono_keeper.DataStoreInternals.archive_visibility_delay_secs = $VISIBILITY_SECS |
    .chrono_keeper.DataStoreInternals.shutdown_confirm_timeout_secs = 30 |
    .chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK_SECS |
    .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT_SECS |
    .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = $REPORT_SECS" \
    "$CONF_TEMPLATE.wal_cap_backup" > "$CONF_TEMPLATE.wal_cap_patched" ||
    { say "conf patch failed; template left as it was"; rm -f "$CONF_TEMPLATE.wal_cap_backup" "$CONF_TEMPLATE.wal_cap_patched"; exit 2; }
mv -f "$CONF_TEMPLATE.wal_cap_patched" "$CONF_TEMPLATE"

RUN_DIR=$(mktemp -d /tmp/keeper_wal_cap.XXXXXX)
restore_conf() { mv -f "$CONF_TEMPLATE.wal_cap_backup" "$CONF_TEMPLATE"; }
remove_wal() {
    rm -rf "$WAL_DIR"
    rmdir -- "$WORK_DIR/wal" 2> /dev/null || true
}
trap 'cleanup; remove_wal; restore_conf; rm -rf "$RUN_DIR"' EXIT
: > "$RUN_DIR/refused_full"

kill_daemons
sleep 2
rm -f "$MONITOR_DIR"/chrono-*.log
rm -rf "${OUTPUT_DIR:?}/$CHRONICLE"
rm -rf "$WAL_DIR"

say "deploying 1 keeper / 1 recording group with the WAL on"
"$DEPLOY" -d -w "$WORK_DIR" -k 1 -r 1 --wal > "$RUN_DIR/deploy.out" 2>&1 ||
    { say "deploy failed"; cat "$RUN_DIR/deploy.out"; exit 2; }
sleep 3
GRAPHER_PID=$(pgrep -f "$WORK_DIR/bin/chrono-grapher --config" | head -1)
if [ -z "$GRAPHER_PID" ] || [ "$(pgrep -c -f "$WORK_DIR/bin/chrono-")" -lt 4 ]; then
    say "expected 4 daemons up"; exit 2
fi
client_conf holder 5580
client_conf burst 5581
client_conf full 5582
client_conf resume 5583
client_conf replay 5584

# The visor notifies the grapher when the story is first acquired. A client
# keeps that acquire, so the burst and the later probes find the story already
# recording and do not call the stopped grapher.
touch "$RUN_DIR/holder_stop"
"$PROBE" --config "$RUN_DIR/holder_client_conf.json" --chronicle "$CHRONICLE" --story "$STORY" \
    --loop --stop-file "$RUN_DIR/holder_stop" --release-file "$RUN_DIR/holder_release" \
    --interval-ms 1000 --timeout-ms 1000 --attempts 1 > "$RUN_DIR/holder.out" 2>&1 &
HOLDER_PID=$!
deadline=$((SECONDS + 30))
while [ $SECONDS -lt $deadline ] && ! grep -q '^LOOP_STOPPED' "$RUN_DIR/holder.out"; do
    kill -0 "$HOLDER_PID" 2> /dev/null || break
    sleep 0.2
done
if ! grep -q '^ACQUIRED CL_SUCCESS' "$RUN_DIR/holder.out"; then
    say "the holding client did not acquire $STORY"; cat "$RUN_DIR/holder.out"; exit 2
fi

say "stopping grapher $GRAPHER_PID so confirmed bytes cannot fall"
kill -STOP "$GRAPHER_PID" || { say "could not stop the grapher"; exit 2; }

# ----------------------------------------------------------- burst ----
write_story burst "$BURST"
burst_full=$(code_count CL_ERR_DURABILITY_FULL)
if [ "$W_ACKED" -eq "$BURST" ] && [ -z "$burst_full" ]; then
    ok "burst: all $BURST durable events acknowledged while the grapher was stopped"
else
    bad "burst: $W_ACKED of $BURST acknowledged, codes ${W_CODES:-none}; the chunk may have sealed during the burst"
fi

say "waiting ${SEAL_WAIT_SECS}s for the burst to seal into an unconfirmed chunk"
sleep "$SEAL_WAIT_SECS"
archived=$(h5_story_events)
if [ "$archived" -eq 0 ]; then
    ok "cap: the archive has no events of $STORY while the grapher is stopped"
else
    bad "cap: the archive already holds $archived event(s); the grapher was not stopped"
fi

# ------------------------------------------------------------- full ----
write_story full 1
full_count=$(code_count CL_ERR_DURABILITY_FULL)
if [ "$W_ACKED" -eq 0 ] && [ "${full_count:-0}" -eq 1 ]; then
    ok "cap: the next durable write got CL_ERR_DURABILITY_FULL"
else
    bad "cap: wanted one CL_ERR_DURABILITY_FULL and no ack, got $W_ACKED acked, codes ${W_CODES:-none}"
fi

# ------------------------------------------------------------ resume ----
say "continuing the grapher"
resume_grapher
GRAPHER_PID=""
resume_acked=0
deadline=$((SECONDS + RESUME_DEADLINE))
while [ $SECONDS -lt $deadline ]; do
    write_story resume 1
    resume_acked=$W_ACKED
    [ "$resume_acked" -eq 1 ] && break
    sleep 2
done
if [ "$resume_acked" -eq 1 ]; then
    ok "cap: a durable write was acknowledged again after the grapher ran"
else
    bad "cap: no durable write was acknowledged within ${RESUME_DEADLINE}s of continuing the grapher (last codes ${W_CODES:-none})"
fi

# ------------------------------------------------------------ replay ----
sort -u "$RUN_DIR/acked_burst" "$RUN_DIR/acked_resume" > "$RUN_DIR/expect.ids"
expected=$(wc -l < "$RUN_DIR/expect.ids")
# the write that was admitted again may still be in an open chunk
missing=-1
twice=-1
stored_refused=-1
deadline=$((SECONDS + 60))
while [ $SECONDS -lt $deadline ]; do
    replay_story
    missing=$(comm -23 "$RUN_DIR/expect.ids" <(sort -u "$RUN_DIR/replay.ids") | wc -l)
    twice=$(awk 'NR == FNR { acked[$1 " " $2] = 1; next } ($1 " " $2) in acked { n[$1 " " $2]++ }
                 END { c = 0; for(k in n) if(n[k] > 1) c++; print c }' \
                "$RUN_DIR/expect.ids" "$RUN_DIR/replay.ids")
    stored_refused=$(awk 'NR == FNR { refused[$1 " " $2] = 1; next } ($2 " " $3) in refused { c++ } END { print c + 0 }' \
                         "$RUN_DIR/refused_full" "$RUN_DIR/replay.ids")
    if [ "$R_STATUS" = "CL_SUCCESS" ] && [ "$missing" -eq 0 ] && [ "$twice" -eq 0 ]; then
        break
    fi
    sleep 2
done
refused=$(wc -l < "$RUN_DIR/refused_full")
unexpected=$(comm -13 "$RUN_DIR/expect.ids" <(sort -u "$RUN_DIR/replay.ids") | wc -l)
if [ "$R_STATUS" = "CL_SUCCESS" ] && [ "$missing" -eq 0 ] && [ "$twice" -eq 0 ] && [ "$refused" -ge 1 ] &&
    [ "$stored_refused" -eq 0 ]; then
    ok "replay: all $expected acknowledged identities came back exactly once, and none of the $refused refused writes ($unexpected unacknowledged)"
else
    bad "replay: status ${R_STATUS:-none}, $missing of $expected acknowledged identities missing, $twice returned more than once, $stored_refused of $refused refused writes returned"
fi

say "${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ]
