#!/bin/bash
# Integration test for the player's watermark-based replay split
# (see watermark_plan/watermark_feedback_impl_plan.md, Task 4).
#
# The player fans story_range_fetch out over the story's keeper roster and
# splits replay at B = min(hot_floor): [start, B) comes from the HDF5 archive,
# [B, end) from the keepers' retained chunks. This script probes one story's
# events through the phases of their life:
#
#   probe 1 (hot):      right after the keepers seal the chunks, before/while
#                       the grapher persists them -> served (at least partly)
#                       by the keeper hot path, exactly N unique events.
#   probe 2 (archived): after the watermark loop freed the tail-released
#                       chunks -> archive covers the range below B, still
#                       exactly N unique events, no duplicates.
#   probe 3 (mixed):    a second writer run doubles the story; the replay must
#                       return exactly 2N unique events (old = archive,
#                       new = hot), and tail playback (playback(10)) still
#                       works off the retention store's index.
#
# The keeper tail_capacity is patched to 20 (< N=30) so the oldest chunk is
# tail-released and actually freed by the watermark loop mid-test — forcing a
# real archive+hot split — while the newest events keep backing tail reads.
#
# Requires an installed tree (deploy_local.sh layout) and the build tree for
# the probe/example binaries. Patches only the *installed* conf template.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        ./watermark_replay_split_test.sh

set -u

WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
BUILD_DIR="${BUILD_DIR:-$HOME/chronolog-build/Debug}"

MONITOR_DIR="$WORK_DIR/monitor"
OUTPUT_DIR="$WORK_DIR/output"
CONF_TEMPLATE="$WORK_DIR/conf/default-chrono-conf.json"
CLIENT_CONF="$WORK_DIR/conf/default-chrono-client-conf.json"
DEPLOY="$WORK_DIR/tools/deploy/deploy_local.sh"
TAIL_EXAMPLE="$BUILD_DIR/client/cpp/examples/chrono-client-example-tail-reader"
REPLAY_CHECK="$BUILD_DIR/tests/integration/watermark-replay/chronolog-test-replay-split-check"

G_CHUNK_SECS=10
G_ACCEPT_SECS=20
REPORT_SECS=1
RESEND_SECS=40
TAIL_CAP=20 # < the 30 events one writer run produces

PASS=0
FAIL=0

say()  { echo -e "[watermark_replay_split_test] $*"; }
ok()   { say "PASS: $*"; PASS=$((PASS + 1)); }
bad()  { say "FAIL: $*"; FAIL=$((FAIL + 1)); }

kill_daemons() {
    for bin in chrono-visor chrono-keeper chrono-grapher chrono-player; do
        pkill -9 -f "$WORK_DIR/bin/$bin" 2> /dev/null
    done
}

cleanup() {
    "$DEPLOY" -s -w "$WORK_DIR" > /dev/null 2>&1
    kill_daemons
}

# The probe is a reader-enabled client: it binds a local query-response
# service at the client conf's QUERY port. The default port may be taken by
# unrelated processes on a dev box, so the probe gets its own conf copy on
# PROBE_QUERY_PORT (default 5561).
PROBE_QUERY_PORT="${PROBE_QUERY_PORT:-5561}"
PROBE_CLIENT_CONF="/tmp/wmark_replay_probe_client_conf.json"

replay_unique() { # runs the probe, echoes the REPLAY_UNIQUE value (or -1); raw output tee'd to $1
    local raw_out="${1:-/dev/null}"
    local out
    out=$("$REPLAY_CHECK" --config "$PROBE_CLIENT_CONF" TailChronicle TailStory 2>&1)
    echo "$out" > "$raw_out"
    local unique
    unique=$(echo "$out" | sed -n 's/^REPLAY_UNIQUE \([0-9]*\)$/\1/p')
    echo "${unique:--1}"
}

hot_event_lines() { # player log lines where the hot path returned events
    grep -c "got [1-9][0-9]* hot events" "$MONITOR_DIR"/chrono-player-1.log 2>/dev/null || true
}

# ---------------------------------------------------------------- setup ----
command -v jq >/dev/null || { say "jq not found"; exit 2; }
[ -x "$TAIL_EXAMPLE" ] || { say "tail-reader example not found at $TAIL_EXAMPLE"; exit 2; }
[ -x "$REPLAY_CHECK" ] || { say "replay probe not found at $REPLAY_CHECK"; exit 2; }

say "patching installed conf template (keeper tail_capacity=$TAIL_CAP, resend=${RESEND_SECS}s; grapher ${G_CHUNK_SECS}s/${G_ACCEPT_SECS}s windows)"
cp "$CONF_TEMPLATE" "$CONF_TEMPLATE.wmark_replay_backup"
jq ".chrono_keeper.DataStoreInternals.tail_capacity = $TAIL_CAP |
    .chrono_keeper.DataStoreInternals.watermark_resend_timeout_secs = $RESEND_SECS |
    .chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK_SECS |
    .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT_SECS |
    .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = $REPORT_SECS" \
    "$CONF_TEMPLATE.wmark_replay_backup" > "$CONF_TEMPLATE" || { say "conf patch failed"; exit 2; }

restore_conf() { mv -f "$CONF_TEMPLATE.wmark_replay_backup" "$CONF_TEMPLATE"; }
trap 'cleanup; restore_conf' EXIT

kill_daemons
sleep 2
rm -f "$MONITOR_DIR"/chrono-keeper-*.log "$MONITOR_DIR"/chrono-grapher-*.log "$MONITOR_DIR"/chrono-player-*.log
rm -f "$OUTPUT_DIR"/TailChronicle.*.h5
# The manifest goes with them. Leaving it behind makes the next run start from a
# manifest describing files that are no longer there, which restores a stale
# watermark and makes the retention assertions below fail for the wrong reason.
rm -f "$OUTPUT_DIR"/archive_manifest*.log "$OUTPUT_DIR"/archive_manifest*.json

say "deploying 2 keepers / 1 recording group"
"$DEPLOY" -d -w "$WORK_DIR" -k 2 -r 1 > /dev/null 2>&1 || { say "deploy failed"; exit 2; }
sleep 3
if [ "$(pgrep -c -f "$WORK_DIR/bin/chrono-")" -lt 5 ]; then
    say "expected 5 daemons up"; exit 2
fi

# probe-private client conf on a free query port (see PROBE_QUERY_PORT above)
jq ".chrono_client.ClientQueryService.rpc.service_base_port = $PROBE_QUERY_PORT" \
    "$CLIENT_CONF" > "$PROBE_CLIENT_CONF" || { say "probe conf patch failed"; exit 2; }

# ------------------------------------------------------ writer run 1 ----
# The tail-reader example writes 30 events, then HOLDS the story acquired for
# 40 s so they seal into the keeper tail, then releases. Probe 1 must run
# during that hold — after the keeper seals (~25 s: 10 s chunk + 15 s
# acceptance) but before the writer releases the story — so the data is hot
# and the story is still live. Anchoring to the "Holding" marker (rather than
# a fixed sleep from launch) keeps probe 1 clear of the release/teardown race.
WRITER1_OUT=/home/kfeng/.claude/jobs/d384eb51/tmp/replay_writer1.out
say "writer run 1: 30 events (tail-reader example, no destroy)"
: > "$WRITER1_OUT"
"$TAIL_EXAMPLE" --config "$CLIENT_CONF" > "$WRITER1_OUT" 2>&1 &
writer_pid=$!

# wait for the writes to complete and the hold to begin
for _ in $(seq 1 30); do
    grep -q "Holding the story acquired" "$WRITER1_OUT" && break
    sleep 1
done
say "writer holding; waiting for the keeper seal, then probing during the hold"
sleep 30 # events sealed (~25s), writer still holds for ~10s more

# ------------------------------------------------- probe 1: hot side ----
u1=$(replay_unique /home/kfeng/.claude/jobs/d384eb51/tmp/replay_probe1.out)
if [ "$u1" -eq 30 ]; then
    ok "probe 1 (hot): replay returned exactly 30 unique events"
else
    say "probe 1 raw output: $(tr '\n' '|' < /home/kfeng/.claude/jobs/d384eb51/tmp/replay_probe1.out)"
    bad "probe 1 (hot): expected 30 unique events, got $u1"
fi
if [ "$(hot_event_lines)" -gt 0 ]; then
    ok "probe 1 (hot): player served events from the keeper hot path"
else
    bad "probe 1 (hot): no hot events in the player log"
fi

wait "$writer_pid" 2>/dev/null

# ------------------------------------- probe 2: watermark-freed side ----
# grapher windows all sealed+persisted by ~(write_end + chunk + acceptance),
# reports at 1 Hz; give the loop time to free the tail-released chunk(s)
say "waiting for the watermark loop to free tail-released chunks"
sleep 70

u2=$(replay_unique)
if [ "$u2" -eq 30 ]; then
    ok "probe 2 (archived): replay still returns exactly 30 unique events"
else
    bad "probe 2 (archived): expected 30 unique events, got $u2"
fi
h5_count=$(ls "$OUTPUT_DIR"/TailChronicle.TailStory.*.h5 2>/dev/null | wc -l)
if [ "$h5_count" -gt 0 ]; then
    ok "probe 2 (archived): $h5_count HDF5 file(s) persisted for the story"
else
    bad "probe 2 (archived): no HDF5 files for the story"
fi
if grep -q "keeper(s)" "$MONITOR_DIR"/chrono-player-1.log 2>/dev/null; then
    ok "roster: player received the story's keeper roster from the visor"
else
    bad "roster: no keeper roster arrival in the player log"
fi

# ------------------------------ probe 3: mixed + tail playback intact ----
# run 2's 30 events evict run 1's from the 20-event tail, tail-releasing the
# run-1 chunks; the watermark (which already covers them) then frees them —
# the archive+hot split below is real, not hot-only
say "writer run 2: 30 more events; also validates tail playback"
"$TAIL_EXAMPLE" --config "$CLIENT_CONF" > /home/kfeng/.claude/jobs/d384eb51/tmp/replay_writer2.out 2>&1
if grep -q "playback(10) returned: CL_SUCCESS with 10 event" /home/kfeng/.claude/jobs/d384eb51/tmp/replay_writer2.out; then
    ok "probe 3 (mixed): tail playback still serves the last-N tail"
else
    bad "probe 3 (mixed): tail playback broken (see replay_writer2.out)"
fi

sleep 40 # let run 2's chunks seal so the replay below can see all of them

freed=$(grep -hc "\[KeeperChunkRetentionStore\] freeing StoryId" "$MONITOR_DIR"/chrono-keeper-*.log 2>/dev/null | awk '{ s += $1 } END { print s + 0 }')
if [ "${freed:-0}" -gt 0 ]; then
    ok "probe 3 (mixed): keepers freed $freed watermark-covered chunk(s) mid-test"
else
    bad "probe 3 (mixed): no chunks were freed — the archive side of the split was never exercised"
fi

u3=$(replay_unique)
if [ "$u3" -eq 60 ]; then
    ok "probe 3 (mixed): replay returns exactly 60 unique events across both runs"
else
    bad "probe 3 (mixed): expected 60 unique events, got $u3"
fi

# the clock-guess split must be gone from the replay path
if grep -q "get_active_window_boundary" "$MONITOR_DIR"/chrono-player-1.log 2>/dev/null; then
    bad "legacy: get_active_window_boundary appears in the player replay log"
else
    ok "legacy: replay no longer uses the clock-guess active-window boundary"
fi

# ------------------------------------------------------------------ report ----
say "${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ]
