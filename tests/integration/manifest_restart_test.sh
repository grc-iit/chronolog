#!/bin/bash
# Integration test for the archive manifest across a Player restart
# (see manifest_plan/manifest_impl_plan.md, step 5).
#
# The manifest exists so the Player does not have to rediscover the archive by
# walking it. That claim has two halves, and this script asserts both against a
# live deployment, because neither is observable from a unit test:
#
#   1. A Player restarted against a populated archive builds its index from the
#      manifest and does NOT run the recursive directory scan.
#   2. Replay returns the same events afterwards as before. A faster startup that
#      quietly loses or duplicates events would be worse than the scan it replaced.
#
# The second is the one that needs a live cluster: the index is only useful if the
# files it names still read back correctly through the whole player query path.
#
# Only the PLAYER is restarted. Restarting everything would let the keepers re-send
# and the grapher re-persist, which would repopulate the index by the ordinary
# runtime path and prove nothing about startup.
#
# Requires an installed tree (deploy_local.sh layout) and the build tree for the
# writer/probe binaries. Patches only the *installed* conf template.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        ./manifest_restart_test.sh

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

PLAYER_BIN="$WORK_DIR/bin/chrono-player"
PLAYER_CONF="$WORK_DIR/conf/chrono-player-conf-1.json"
PLAYER_LOG="$MONITOR_DIR/chrono-player-1.log"
# A glob, not a name: each Grapher writes its own archive_manifest.<group>.log
# (see ArchiveManifest / manifest_plan/nfs_validation.md).
MANIFEST_GLOB="$OUTPUT_DIR/archive_manifest*.log"

# Short grapher windows so a chunk is sealed, persisted and recorded within the
# test rather than minutes later.
G_CHUNK_SECS=10
G_ACCEPT_SECS=20
REPORT_SECS=1
ARCHIVE_WAIT_SECS=150

PASS=0
FAIL=0

say() { echo -e "[manifest_restart_test] $*"; }
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

PROBE_QUERY_PORT="${PROBE_QUERY_PORT:-5563}"
PROBE_CLIENT_CONF="/tmp/manifest_restart_probe_client_conf.json"

replay_unique() { # echoes the REPLAY_UNIQUE value, or -1
    local out
    out=$("$REPLAY_CHECK" --config "$PROBE_CLIENT_CONF" TailChronicle TailStory 2>&1)
    echo "$out" > "${1:-/dev/null}"
    local unique
    unique=$(echo "$out" | sed -n 's/^REPLAY_UNIQUE \([0-9]*\)$/\1/p')
    echo "${unique:--1}"
}

published_records() { # manifest records that name a file, across every writer's log
    # shellcheck disable=SC2086
    grep -h '"state":"published"' $MANIFEST_GLOB 2> /dev/null | wc -l
}

# ---------------------------------------------------------------- setup ----
command -v jq > /dev/null || {
    say "jq not found"
    exit 2
}
[ -x "$TAIL_EXAMPLE" ] || {
    say "tail-reader example not found at $TAIL_EXAMPLE"
    exit 2
}
[ -x "$REPLAY_CHECK" ] || {
    say "replay probe not found at $REPLAY_CHECK"
    exit 2
}

say "patching installed conf template (grapher ${G_CHUNK_SECS}s/${G_ACCEPT_SECS}s windows)"
cp "$CONF_TEMPLATE" "$CONF_TEMPLATE.manifest_restart_backup"
jq ".chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK_SECS |
    .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT_SECS |
    .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = $REPORT_SECS" \
    "$CONF_TEMPLATE.manifest_restart_backup" > "$CONF_TEMPLATE" || {
    say "conf patch failed"
    exit 2
}

restore_conf() { mv -f "$CONF_TEMPLATE.manifest_restart_backup" "$CONF_TEMPLATE"; }
trap 'cleanup; restore_conf' EXIT

kill_daemons
sleep 2
rm -f "$MONITOR_DIR"/chrono-keeper-*.log "$MONITOR_DIR"/chrono-grapher-*.log "$MONITOR_DIR"/chrono-player-*.log
rm -f "$OUTPUT_DIR"/TailChronicle.*.h5 "$OUTPUT_DIR"/archive_manifest*.log "$OUTPUT_DIR"/archive_manifest*.json

say "deploying 1 keeper / 1 recording group"
"$DEPLOY" -d -w "$WORK_DIR" -k 1 -r 1 > /dev/null 2>&1 || {
    say "deploy failed"
    exit 2
}
sleep 3

jq ".chrono_client.ClientQueryService.rpc.service_base_port = $PROBE_QUERY_PORT" \
    "$CLIENT_CONF" > "$PROBE_CLIENT_CONF" || {
    say "probe conf patch failed"
    exit 2
}

# ------------------------------------------------------------- publish ----
say "writing 30 events and waiting for them to reach the archive"
timeout 200 "$TAIL_EXAMPLE" --config "$CLIENT_CONF" > /tmp/manifest_restart_writer.out 2>&1

waited=0
while [ "$waited" -lt "$ARCHIVE_WAIT_SECS" ]; do
    if [ "$(published_records)" -gt 0 ] && ls "$OUTPUT_DIR"/TailChronicle.*.h5 > /dev/null 2>&1; then
        break
    fi
    sleep 5
    waited=$((waited + 5))
done

h5_count=$(ls -1 "$OUTPUT_DIR"/TailChronicle.*.h5 2> /dev/null | wc -l)
if [ "$(published_records)" -gt 0 ] && [ "$h5_count" -gt 0 ]; then
    ok "publish: $h5_count HDF5 file(s) archived and $(published_records) published manifest record(s) written"
else
    bad "publish: nothing reached the archive within ${ARCHIVE_WAIT_SECS}s (h5=$h5_count, published=$(published_records))"
    say "result: $PASS passed, $FAIL failed"
    exit 1
fi

BEFORE=$(replay_unique /tmp/manifest_restart_before.out)
if [ "$BEFORE" -gt 0 ]; then
    ok "replay before restart: $BEFORE unique event(s)"
else
    bad "replay before restart returned no events (probe said '$BEFORE'), nothing to compare against"
    say "result: $PASS passed, $FAIL failed"
    exit 1
fi

# ------------------------------------------------- restart just the player ----
# The log is moved aside rather than truncated so the restarted player's startup
# lines cannot be confused with the original deployment's.
say "restarting ONLY the player"
pkill -9 -f "$WORK_DIR/bin/chrono-player" 2> /dev/null
sleep 3
mv -f "$PLAYER_LOG" "$PLAYER_LOG.before_restart" 2> /dev/null
(cd "$WORK_DIR" && setsid "$PLAYER_BIN" --config "$PLAYER_CONF" > /dev/null 2>&1 < /dev/null &)
sleep 12

if ! pgrep -f "$WORK_DIR/bin/chrono-player" > /dev/null; then
    bad "restart: the player did not come back up"
    say "result: $PASS passed, $FAIL failed"
    exit 1
fi

# --------------------------------------------------------- the assertions ----
if grep -q "(no directory scan)" "$PLAYER_LOG" 2> /dev/null; then
    indexed_line=$(grep -h "(no directory scan)" "$PLAYER_LOG" | tail -1)
    ok "restart: index built from the manifest -- ${indexed_line#*\[HDF5ArchiveReadingAgent\] }"
else
    bad "restart: the player did not build its index from the manifest"
fi

# The scan is the thing the manifest replaces; its own log line is the evidence.
if grep -qE "falling back to a recursive|Created start_time_file_name_map_" "$PLAYER_LOG" 2> /dev/null; then
    bad "restart: the player still ran a recursive directory scan"
else
    ok "restart: no recursive directory scan ran"
fi

indexed_files=$(grep -h "(no directory scan)" "$PLAYER_LOG" 2> /dev/null | sed -n 's/.*Indexed \([0-9]*\) archive file.*/\1/p' | tail -1)
if [ -n "$indexed_files" ] && [ "$indexed_files" -ge "$h5_count" ]; then
    ok "restart: indexed $indexed_files archive file(s) from the manifest (>= the $h5_count on disk)"
else
    bad "restart: indexed '${indexed_files:-none}' archive file(s), expected at least $h5_count"
fi

AFTER=$(replay_unique /tmp/manifest_restart_after.out)
if [ "$AFTER" = "$BEFORE" ]; then
    ok "replay after restart: $AFTER unique event(s), unchanged across the restart"
else
    bad "replay after restart returned $AFTER unique event(s), expected $BEFORE"
fi

# ------------------------------------- the tail poll, on a manifest-mode player ----
# Everything above restarted a Player against an archive that already existed. The
# other half of the claim is that a RUNNING manifest-mode Player keeps up as the
# Grapher publishes, by re-reading the tail of the log rather than re-walking the
# archive. The Player is now in manifest mode (asserted above), so a second writer
# run exercises exactly that path.
say "writing 30 more events; the running player must pick them up from the manifest tail"
before_files=$h5_count
timeout 200 "$TAIL_EXAMPLE" --config "$CLIENT_CONF" > /tmp/manifest_restart_writer2.out 2>&1

waited=0
while [ "$waited" -lt "$ARCHIVE_WAIT_SECS" ]; do
    now_files=$(ls -1 "$OUTPUT_DIR"/TailChronicle.*.h5 2> /dev/null | wc -l)
    if [ "$now_files" -gt "$before_files" ]; then
        break
    fi
    sleep 5
    waited=$((waited + 5))
done

now_files=$(ls -1 "$OUTPUT_DIR"/TailChronicle.*.h5 2> /dev/null | wc -l)
if [ "$now_files" -gt "$before_files" ]; then
    ok "tail poll: a second file was archived ($before_files -> $now_files)"
else
    bad "tail poll: no new file was archived within ${ARCHIVE_WAIT_SECS}s, nothing to pick up"
fi

# Give the poll a few ticks (manifest_poll_interval_ms defaults to 1000).
sleep 6

GREW=$(replay_unique /tmp/manifest_restart_grew.out)
if [ "$GREW" -gt "$BEFORE" ]; then
    ok "tail poll: replay now returns $GREW unique event(s), up from $BEFORE, with no restart"
else
    bad "tail poll: replay still returns $GREW unique event(s); the new file never entered the index"
fi

# The whole point is that it kept up WITHOUT falling back to walking the archive.
if grep -qE "falling back to a recursive|Created start_time_file_name_map_" "$PLAYER_LOG" 2> /dev/null; then
    bad "tail poll: the player fell back to a recursive directory scan"
else
    ok "tail poll: still no recursive directory scan"
fi

say "result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
