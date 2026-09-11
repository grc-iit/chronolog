#!/bin/bash
# Integration test for the keeper <-> grapher watermark feedback loop.
#
# Scenarios (the protocol is described in watermark_plan/watermark_protocol.md):
#   1. Normal path: a writer runs twice; afterwards every chunk a keeper
#      retained is freed within the deadline (ship-on-seal -> grapher persists
#      -> watermark report -> gated eviction), except the one holding that
#      keeper's newest event of the story. Keeper tail_capacity is set to 1,
#      the smallest the config accepts, so the tail keeps only that chunk and
#      every other one is freed by the watermark gate alone.
#   2. No premature free: every "freeing" log line's triggering watermark W
#      must be >= the freed chunk's end time.
#   3. Transient grapher outage: SIGSTOP the grapher before a writer runs,
#      SIGCONT it later; keepers must retain the chunks through the outage,
#      the loop must catch up afterwards (every chunk below the tail freed),
#      and every written event must reach HDF5 (duplicates allowed, loss is
#      failure).
#
# The writer is the tail-reader example (30 events, release, NO destroy).
# chrono-bench is deliberately not used here: it destroys its stories
# immediately, destroy races ahead of the keepers' seal delay, the grapher
# tombstones the story and discards the late chunks — correct destroy
# semantics (the user discarded the data), but then no watermark ever covers
# those chunks. Keepers retain acked-but-never-covered chunks of destroyed
# stories indefinitely (bounded per story) — a documented limitation of the
# watermark protocol, not something this test can green-light around.
#
# Requires an installed tree (deploy_local.sh layout). The script patches the
# *installed* conf template (never the source tree), deploys 2 keepers / 1
# recording group, runs the scenarios, and stops the deployment.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        ./watermark_loop_test.sh

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
TAIL_EXAMPLE="$BUILD_DIR/client/cpp/examples/chrono-client-example-tail-reader"

# grapher windows patched small so the whole loop closes quickly
G_CHUNK_SECS=10
G_ACCEPT_SECS=20
REPORT_SECS=1
RESEND_SECS=40
# keeper seal delay (template: 10s chunks + 15s acceptance) + grapher loop
FREE_DEADLINE=$((25 + 2 * (G_ACCEPT_SECS + G_CHUNK_SECS + REPORT_SECS) + 10))

PASS=0
FAIL=0

say()  { echo -e "[watermark_loop_test] $*"; }
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

# retained/freed line counts for one keeper log
retained_count() { grep -c "\[KeeperChunkRetentionStore\] retaining StoryId" "$1" 2>/dev/null || true; }
freed_count()    { grep -c "\[KeeperChunkRetentionStore\] freeing StoryId" "$1" 2>/dev/null || true; }

totals() { # -> "retained freed" summed over keepers
    local r=0 f=0 log
    for log in "$MONITOR_DIR"/chrono-keeper-*.log; do
        [ -f "$log" ] || continue
        r=$((r + $(retained_count "$log")))
        f=$((f + $(freed_count "$log")))
    done
    echo "$r $f"
}

# chunks a keeper retained and has not freed, other than the one holding its
# newest event of each story -- the only chunk a 1-event tail may keep
unfreed_below_tail() {
    awk '/\[KeeperChunkRetentionStore\] retaining StoryId/ {
             match($0, /StoryId=[0-9]+/); s = substr($0, RSTART + 8, RLENGTH - 8)
             match($0, /chunk [0-9]+-[0-9]+/); c = substr($0, RSTART + 6, RLENGTH - 6)
             kept[s " " c] = 1
             split(c, bounds, "-")
             if(!(s in newest) || bounds[1] + 0 > newest_start[s]) { newest[s] = c; newest_start[s] = bounds[1] + 0 }
         }
         /\[KeeperChunkRetentionStore\] freeing StoryId/ {
             match($0, /StoryId=[0-9]+/); s = substr($0, RSTART + 8, RLENGTH - 8)
             match($0, /chunk [0-9]+-[0-9]+/); delete kept[s " " substr($0, RSTART + 6, RLENGTH - 6)]
         }
         END { n = 0; for(k in kept) { split(k, key, " "); if(key[2] != newest[key[1]]) n++ } print n }' "$1"
}

wait_freed_but_tail() { # $1 = deadline seconds from now; passes once no keeper holds a chunk below its tail
    local deadline=$((SECONDS + $1)) log all
    while [ $SECONDS -lt $deadline ]; do
        all=1
        for log in "$MONITOR_DIR"/chrono-keeper-*.log; do
            [ -f "$log" ] || continue
            [ "$(unfreed_below_tail "$log")" -ne 0 ] && all=0
        done
        [ $all -eq 1 ] && return 0
        sleep 5
    done
    return 1
}

h5_story_events() { # total events across TailChronicle.TailStory files
    local total=0 f n
    for f in "$OUTPUT_DIR"/TailChronicle.TailStory.*.h5; do
        [ -f "$f" ] || continue
        n=$("$H5DUMP" -H -d /story_chunks/data.vlen_bytes "$f" 2>/dev/null |
            sed -n 's/.*DATASPACE *SIMPLE *{ *( *\([0-9]*\) *).*/\1/p' | head -1)
        total=$((total + ${n:-0}))
    done
    echo "$total"
}

# ---------------------------------------------------------------- setup ----
say "patching installed conf template (keeper tail_capacity=1, resend=${RESEND_SECS}s; grapher ${G_CHUNK_SECS}s/${G_ACCEPT_SECS}s windows)"
command -v jq >/dev/null || { say "jq not found"; exit 2; }
[ -x "$TAIL_EXAMPLE" ] || { say "tail-reader example not found at $TAIL_EXAMPLE"; exit 2; }
[ -x "$H5DUMP" ] || { say "h5dump not found at $H5DUMP"; exit 2; }

cp "$CONF_TEMPLATE" "$CONF_TEMPLATE.wmark_test_backup"
jq ".chrono_keeper.DataStoreInternals.tail_capacity = 1 |
    .chrono_keeper.DataStoreInternals.watermark_resend_timeout_secs = $RESEND_SECS |
    .chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK_SECS |
    .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT_SECS |
    .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = $REPORT_SECS" \
    "$CONF_TEMPLATE.wmark_test_backup" > "$CONF_TEMPLATE" || { say "conf patch failed"; exit 2; }

restore_conf() { mv -f "$CONF_TEMPLATE.wmark_test_backup" "$CONF_TEMPLATE"; }
trap 'cleanup; restore_conf' EXIT

kill_daemons
sleep 2
rm -f "$MONITOR_DIR"/chrono-keeper-*.log "$MONITOR_DIR"/chrono-grapher-*.log
rm -f "$OUTPUT_DIR"/TailChronicle.*.h5

say "deploying 2 keepers / 1 recording group"
"$DEPLOY" -d -w "$WORK_DIR" -k 2 -r 1 > /dev/null 2>&1 || { say "deploy failed"; exit 2; }
sleep 3
if [ "$(pgrep -c -f "$WORK_DIR/bin/chrono-")" -lt 5 ]; then
    say "expected 5 daemons up"; exit 2
fi

# ------------------------------------------------- scenario 1: normal path ----
say "scenario 1: write 30 events twice (no destroy), wait up to ${FREE_DEADLINE}s for keepers to free all but their tail chunk"
# Two runs, so every keeper retains a chunk besides the one its 1-event tail
# keeps. The example holds the story ~40 s after writing, so the second run's
# events land in later chunks.
"$TAIL_EXAMPLE" --config "$CLIENT_CONF" > /dev/null 2>&1
"$TAIL_EXAMPLE" --config "$CLIENT_CONF" > /dev/null 2>&1

if wait_freed_but_tail "$FREE_DEADLINE"; then
    read -r r f <<< "$(totals)"
    if [ "$f" -gt 0 ]; then
        ok "normal path: $f of $r retained chunk(s) freed by watermark reports; each keeper keeps only its tail chunk"
    else
        bad "normal path: nothing was freed (retained=$r); the writer runs left no chunk below the tail"
    fi
else
    read -r r f <<< "$(totals)"
    bad "normal path: retained=$r freed=$f after ${FREE_DEADLINE}s deadline; chunks below the tail still held"
fi

# --------------------------------------------- scenario 2: no premature free ----
# every freeing line prints "chunk <start>-<end> eventCount ... (W=<w> covers it)"
premature=$(grep -h "\[KeeperChunkRetentionStore\] freeing StoryId" "$MONITOR_DIR"/chrono-keeper-*.log 2>/dev/null |
    sed -n 's/.*chunk [0-9]*-\([0-9]*\) eventCount.*(W=\([0-9]*\) covers it).*/\1 \2/p' |
    awk '$2 < $1 { count++ } END { print count + 0 }')
if [ "${premature:-0}" -eq 0 ]; then
    ok "no premature free: every freed chunk's W >= its end time"
else
    bad "premature free: $premature freed chunk(s) with W below their end time"
fi

# --------------------------------------- scenario 3: transient grapher outage ----
say "scenario 3: SIGSTOP grapher, write through the outage, SIGCONT, verify catch-up + durability"
grapher_pid=$(pgrep -f "$WORK_DIR/bin/chrono-grapher" | head -1)
[ -n "$grapher_pid" ] || { bad "outage: no grapher pid"; exit 1; }

read -r base_retained base_freed <<< "$(totals)"

# the writer must acquire + write while the grapher is still responsive (the
# visor notifies the grapher at acquire); the keeper seals the chunks ~25s in
# (10s chunks + 15s acceptance), so stopping the grapher at +8s puts the
# outage squarely across the keeper->grapher transfer
say "running the writer; grapher will be stopped during the transfer window"
"$TAIL_EXAMPLE" --config "$CLIENT_CONF" > /dev/null 2>&1 &
example_pid=$!
sleep 8

kill -STOP "$grapher_pid"
say "grapher $grapher_pid stopped"
sleep 45
read -r outage_retained _ <<< "$(totals)"
if [ "$outage_retained" -gt "$base_retained" ]; then
    ok "outage: keepers retained new chunk(s) while the grapher was stopped"
else
    bad "outage: no new retained chunks observed during the outage (retained=$outage_retained base=$base_retained)"
fi

kill -CONT "$grapher_pid"
say "grapher resumed; waiting for re-send + persistence + reports"
wait "$example_pid" 2>/dev/null

if wait_freed_but_tail $((RESEND_SECS + FREE_DEADLINE + 30)); then
    ok "outage: every chunk below each keeper's tail freed after the grapher resumed"
else
    for log in "$MONITOR_DIR"/chrono-keeper-*.log; do
        say "  $(basename "$log"): retained=$(retained_count "$log") freed=$(freed_count "$log") held below tail=$(unfreed_below_tail "$log")"
    done
    bad "outage: keepers still hold chunks below the tail after the catch-up deadline"
fi

# durability: the three runs wrote 30 events each; every one must reach disk
# (TailChronicle.TailStory.*.vlen.h5, possibly rotated; duplicates allowed).
# The tail keeps each keeper's newest chunk in memory whether or not it is
# persisted, so poll the archive rather than infer it from frees; the last
# run's chunks reach the grapher only through the re-send after the outage.
deadline=$((SECONDS + RESEND_SECS + FREE_DEADLINE + 30))
h5_events=$(h5_story_events)
while [ "$h5_events" -lt 90 ] && [ $SECONDS -lt $deadline ]; do
    sleep 5
    h5_events=$(h5_story_events)
done
if [ "$h5_events" -ge 90 ]; then
    ok "durability: $h5_events event(s) on disk for TailStory (>= 90 written; duplicates allowed)"
else
    bad "durability: only $h5_events event(s) on disk for TailStory, 90 were written across three runs"
fi

# ------------------------------------------------------------------ report ----
say "${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ]
