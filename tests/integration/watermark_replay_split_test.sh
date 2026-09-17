#!/bin/bash
# Integration test for the player's watermark-based replay split
# (see docs/versioned_docs/version-3.2.0/user-guide/architecture/durable-retention.md).
#
# The player fans story_range_fetch out over the story's keeper roster and
# splits replay at B, the highest persisted watermark any keeper reports:
# [start, B) comes from the HDF5 archive, [B, end) from the keepers' retained
# chunks, plus any retained events the grapher has not confirmed written. This
# script probes one story's events through the phases of their life:
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
# The keeper tail_capacity is patched to 10, below the ~15 events each of the
# 2 keepers gets from a writer run of N=30, so the oldest chunk is
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
# Wide enough that the grapher cannot have written anything by the time probe 1
# runs: probe 1 has to find the events while they are still only in keeper
# memory, and the whole write -> report -> visibility -> free chain otherwise
# races the fixed sleep below. The first window is written G_CHUNK + G_ACCEPT
# after the events, so this keeps ~25 s of margin.
G_ACCEPT_SECS=45
REPORT_SECS=1
RESEND_SECS=40
VISIBILITY_SECS=5
# a chunk is freed once its window is written, reported, and the visibility
# delay has passed; derived here so the wait follows the knobs above
WAIT_FREE_SECS=$((G_CHUNK_SECS + G_ACCEPT_SECS + REPORT_SECS + VISIBILITY_SECS + 10))
TAIL_CAP=10 # < the ~15 events each of the 2 keepers gets from one writer run
WRITER_EVENTS=30 # what the tail-reader example writes in one run

PASS=0
FAIL=0

say()  { echo -e "[watermark_replay_split_test $(date +%H:%M:%S)] $*"; }
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
# writer and probe output, removed on exit
RUN_DIR=$(mktemp -d /tmp/wmark_replay.XXXXXX)

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
    .chrono_keeper.DataStoreInternals.archive_visibility_delay_secs = $VISIBILITY_SECS |
    .chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK_SECS |
    .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT_SECS |
    .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = $REPORT_SECS" \
    "$CONF_TEMPLATE.wmark_replay_backup" > "$CONF_TEMPLATE" || { say "conf patch failed"; exit 2; }

restore_conf() { mv -f "$CONF_TEMPLATE.wmark_replay_backup" "$CONF_TEMPLATE"; }
trap 'cleanup; restore_conf; rm -rf "$RUN_DIR"' EXIT

kill_daemons
sleep 2
rm -f "$MONITOR_DIR"/chrono-keeper-*.log "$MONITOR_DIR"/chrono-grapher-*.log "$MONITOR_DIR"/chrono-player-*.log
rm -f "$OUTPUT_DIR"/TailChronicle.*.h5

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
WRITER1_OUT="$RUN_DIR/replay_writer1.out"
say "writer run 1: 30 events (tail-reader example, no destroy)"
: > "$WRITER1_OUT"
"$TAIL_EXAMPLE" --config "$CLIENT_CONF" > "$WRITER1_OUT" 2>&1 &
writer_pid=$!

# Probe 1 does not wait for the example's "Holding" line: the example tail-reads
# for about 90 s before printing it, by which time the grapher has written the
# story and the keepers have let go, so the hot path has nothing left to serve.
# The keepers' own seal marker is the signal probe 1 actually depends on, and it
# arrives while the example is still tail-reading -- with the story acquired by
# the writer throughout, which is what probe 1 needs.
# Anchored to the seal itself, not to a sleep from the "Holding" line: the
# example tail-reads for a while before it prints that, so a fixed wait puts
# probe 1 an unknown distance from the events -- far enough, as it turned out,
# for the grapher to have written and the keepers to have freed them first.
# The probe needs about 90 s to connect and acquire before it can replay, which
# is longer than the events stay hot -- so it is started now and held at its
# pause point, and released the moment the keepers have sealed everything. That
# puts the replay itself within a second of the seal, whatever the client's
# startup costs.
say "waiting for the writer to create and acquire the story"
for _ in $(seq 1 120); do
    grep -q "AcquireStory returned: CL_SUCCESS" "$WRITER1_OUT" 2> /dev/null && break
    sleep 1
done

PROBE1_GO="$RUN_DIR/probe1_go"
rm -f "$PROBE1_GO"
: > "$RUN_DIR/replay_probe1.out"
REPLAY_SPLIT_PAUSE_FILE="$PROBE1_GO" "$REPLAY_CHECK" --config "$PROBE_CLIENT_CONF" TailChronicle TailStory \
    > "$RUN_DIR/replay_probe1.out" 2>&1 &
probe1_pid=$!
say "probe 1 starting; waiting for it to acquire the story"
for _ in $(seq 1 180); do
    grep -q '^ACQUIRED$' "$RUN_DIR/replay_probe1.out" 2> /dev/null && break
    sleep 1
done

say "waiting for the keepers to seal all $WRITER_EVENTS events, then releasing probe 1 while they are still hot"
sealed_events=0
for _ in $(seq 1 90); do
    sealed_events=$(grep -h 'retaining StoryId' "$MONITOR_DIR"/chrono-keeper-*.log 2> /dev/null |
        sed -n 's/.*eventCount \([0-9]*\).*/\1/p' | awk '{total += $1} END {print total + 0}')
    [ "$sealed_events" -ge "$WRITER_EVENTS" ] && break
    sleep 1
done
say "keepers have sealed $sealed_events event(s)"
if [ "$sealed_events" -lt "$WRITER_EVENTS" ]; then
    say "only $sealed_events of $WRITER_EVENTS events sealed; probing anyway"
fi
: > "$PROBE1_GO"
wait "$probe1_pid" 2> /dev/null

# ------------------------------------------------- probe 1: hot side ----
# The premise: nothing of this story is on disk yet, so whatever the replay
# returns came from the keepers. Checked rather than assumed -- when it does not
# hold, the failure is that probe 1 ran too late, not that the hot path is broken.
if [ "$(find "$OUTPUT_DIR" -name 'TailChronicle.*.h5' 2>/dev/null | wc -l)" -ne 0 ]; then
    bad "probe 1 (hot): the grapher already archived the story; probe 1 ran too late to exercise the hot path"
fi
u1=$(sed -n 's/^REPLAY_UNIQUE \([0-9]*\)$/\1/p' "$RUN_DIR/replay_probe1.out" | head -1)
u1=${u1:--1}
if [ "$u1" -eq 30 ]; then
    ok "probe 1 (hot): replay returned exactly 30 unique events"
else
    say "probe 1 raw output: $(tr '\n' '|' < "$RUN_DIR/replay_probe1.out")"
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
# reports at 1 Hz, then the visibility delay; give the loop that long to free
# the tail-released chunk(s)
say "waiting ${WAIT_FREE_SECS}s for the watermark loop to free tail-released chunks"
sleep "$WAIT_FREE_SECS"

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
# run 2's events evict run 1's from each keeper's 10-event tail, tail-releasing the
# run-1 chunks; the watermark (which already covers them) then frees them —
# the archive+hot split below is real, not hot-only
say "writer run 2: 30 more events; also validates tail playback"
"$TAIL_EXAMPLE" --config "$CLIENT_CONF" > "$RUN_DIR/replay_writer2.out" 2>&1
if grep -q "playback(10) returned: CL_SUCCESS with 10 event" "$RUN_DIR/replay_writer2.out"; then
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
