#!/bin/bash
# End-to-end check that a replay splits exactly where the watermark design says
# (see docs/versioned_docs/version-3.2.0/user-guide/architecture/durable-retention.md).
#
# A client writes bursts of events; they flow keeper -> grapher -> HDF5 -> player
# and come back through ReplayStory. The player splits a replay at B, the highest
# persisted watermark W any keeper reports: the archive serves [start, B) and the
# keepers [B, end). With the timing settings pinned below, the moment each window
# is written is known, so the script predicts B and which events come from where,
# and probes only at times that stay clear of the next change:
#
#   phase 1 (hot)     burst A, probed after the keepers sealed it and before the
#                     grapher writes its window: every event from the keepers
#   phase 2 (cold)    after A's window is written, reported and past the keepers'
#                     visibility delay: every event from the archive, none from
#                     the keepers, B = end of the empty window after A's
#   phase 3 (split)   burst C in a later window: A from the archive, C from the
#                     keepers, B = start of C's window
#   phase 4 (frozen)  burst D, then the grapher is stopped (SIGSTOP) after it has
#                     received D but before it writes D's window: W stops moving,
#                     D stays on the keepers, B does not move between two probes
#   phase 5 (resumed) SIGCONT: D's window is written, every event from the archive
#
# Each probe compares the replayed events with the written ones by timestamp
# (exact set, no duplicates) and reads B and the keepers' share from the player's
# per-query log line. The HDF5 files are checked at the end: one per window that
# held events, and no numbered (rewritten) file.
#
# Timing (seconds; patched into the installed conf template and restored):
#   keeper  window K=5, acceptance AK=3   -> a chunk seals at most K+AK after its first event
#   grapher window G=10, acceptance AG=15 -> window [s, s+10) is written at s+25
#   report interval 1, keeper visibility delay 3
# The grapher keeps at least two windows open and writes each one, empty or not,
# once it is AG past its end, so W at time t is the end of the last window
# [s, s+G) with s + G + AG <= t. A probe at t with (t - AG) mod G = 4 sees
# B = t - AG - 4 even with up to ~3 s of loop and report latency; probes are
# placed at window start + 19 for that reason. A keeper treats a written chunk
# as served by the archive only the visibility delay after its receipt settles,
# so the cold probe waits one more window.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        ./watermark_exact_split_test.sh

set -u

WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
BUILD_DIR="${BUILD_DIR:-$HOME/chronolog-build/Debug}"

MONITOR_DIR="$WORK_DIR/monitor"
OUTPUT_DIR="$WORK_DIR/output"
CONF_TEMPLATE="$WORK_DIR/conf/default-chrono-conf.json"
CLIENT_CONF="$WORK_DIR/conf/default-chrono-client-conf.json"
DEPLOY="$WORK_DIR/tools/deploy/deploy_local.sh"
PROBE="$BUILD_DIR/tests/integration/watermark-replay/chronolog-test-exact-split-probe"

CHRONICLE=ExactSplitChronicle
STORY=ExactSplitStory
BURST=20

K_CHUNK=5
K_ACCEPT=3
G_CHUNK=10
G_ACCEPT=15
REPORT_SECS=1
VISIBILITY_SECS=3
RESEND_SECS=120 # far beyond the run's waits: nothing is sent twice
PROBE_OFFSET=19 # probe at window start + 19: (t - G_ACCEPT) mod G_CHUNK = 4

PASS=0
FAIL=0

say() { echo -e "[watermark_exact_split_test $(date +%H:%M:%S)] $*"; }
ok() { say "PASS: $*"; PASS=$((PASS + 1)); }
bad() { say "FAIL: $*"; FAIL=$((FAIL + 1)); }

kill_daemons() {
    for bin in chrono-visor chrono-keeper chrono-grapher chrono-player; do
        pkill -CONT -f "$WORK_DIR/bin/$bin" 2> /dev/null
        pkill -9 -f "$WORK_DIR/bin/$bin" 2> /dev/null
    done
}

cleanup() {
    pkill -CONT -f "$WORK_DIR/bin/chrono-grapher" 2> /dev/null
    [ -n "${holder_pid:-}" ] && kill "$holder_pid" 2> /dev/null
    "$DEPLOY" -s -w "$WORK_DIR" > /dev/null 2>&1
    kill_daemons
}

now_ns() { date +%s%N; }
sec_of() { echo $(($1 / 1000000000)); }

wait_until_sec() { # sleeps until the wall clock reaches the given epoch second
    local target_ns=$(($1 * 1000000000))
    while [ "$(now_ns)" -lt "$target_ns" ]; do sleep 0.1; done
}

next_window_start() { # first window start (multiple of G) at or after the given second
    local t=$1
    echo $(((t + G_CHUNK - 1) / G_CHUNK * G_CHUNK))
}

# Writer and replay probe each bind a local query-response service, on ports of
# their own: in phase 4 the writer holds the story while the probe replays it,
# and two clients on one port cannot both start.
PROBE_QUERY_PORT="${PROBE_QUERY_PORT:-5563}"
WRITER_QUERY_PORT="${WRITER_QUERY_PORT:-5564}"
RUN_DIR=$(mktemp -d /tmp/wmark_exact.XXXXXX)
PROBE_CLIENT_CONF="$RUN_DIR/probe_client_conf.json"
WRITER_CLIENT_CONF="$RUN_DIR/writer_client_conf.json"
WRITTEN="$RUN_DIR/written" # every timestamp written so far, one per line

write_burst() { # $1 = label, $2 = hold seconds (0: release right away); appends to $WRITTEN
    local out="$RUN_DIR/write_$1.out"
    if [ "$2" -gt 0 ]; then
        "$PROBE" --config "$WRITER_CLIENT_CONF" write "$CHRONICLE" "$STORY" "$BURST" "$2" > "$out" 2>&1 &
        holder_pid=$!
        for _ in $(seq 1 100); do grep -q '^WRITTEN' "$out" && break; sleep 0.1; done
    else
        "$PROBE" --config "$WRITER_CLIENT_CONF" write "$CHRONICLE" "$STORY" "$BURST" > "$out" 2>&1
    fi
    sed -n 's/^WROTE \([0-9]*\)$/\1/p' "$out" > "$RUN_DIR/burst_$1"
    cat "$RUN_DIR/burst_$1" >> "$WRITTEN"
    if [ "$(wc -l < "$RUN_DIR/burst_$1")" -ne "$BURST" ]; then
        say "burst $1 wrote $(wc -l < "$RUN_DIR/burst_$1") of $BURST events (see $out)"
        return 1
    fi
}

burst_window() { # the window start (seconds) of a burst; fails if it straddles two windows
    local first last
    first=$(sort -n "$RUN_DIR/burst_$1" | head -1)
    last=$(sort -n "$RUN_DIR/burst_$1" | tail -1)
    local w_first=$(($(sec_of "$first") / G_CHUNK * G_CHUNK))
    local w_last=$(($(sec_of "$last") / G_CHUNK * G_CHUNK))
    [ "$w_first" -eq "$w_last" ] || { say "burst $1 straddles windows $w_first and $w_last"; return 1; }
    echo "$w_first"
}

player_split_lines() { cat "$MONITOR_DIR"/chrono-player-*.log 2> /dev/null | grep -c 'hot_boundary' || true; }

# probe <label>: replays the story; sets P_STATUS, P_EVENTS (file), P_B (ns), P_HOT
probe() {
    local label=$1
    local before
    before=$(player_split_lines)
    "$PROBE" --config "$PROBE_CLIENT_CONF" replay "$CHRONICLE" "$STORY" > "$RUN_DIR/replay_$label.out" 2>&1
    P_STATUS=$(sed -n 's/^REPLAY_STATUS \(.*\)$/\1/p' "$RUN_DIR/replay_$label.out")
    P_EVENTS="$RUN_DIR/replay_$label.events"
    sed -n 's/^EVENT \([0-9]*\) .*$/\1/p' "$RUN_DIR/replay_$label.out" > "$P_EVENTS"
    local line=""
    for _ in $(seq 1 20); do # the player logs the line before it answers; allow for the flush
        if [ "$(player_split_lines)" -gt "$before" ]; then
            line=$(cat "$MONITOR_DIR"/chrono-player-*.log | grep 'hot_boundary' | tail -1)
            break
        fi
        sleep 0.2
    done
    P_B=$(echo "$line" | sed -n 's/.*hot_boundary \([0-9]*\) hot_events.*/\1/p')
    P_HOT=$(echo "$line" | sed -n 's/.*hot_events \([0-9]*\).*/\1/p')
    say "probe $label: status ${P_STATUS:-?}, $(wc -l < "$P_EVENTS") event(s), B=${P_B:-?} ($(sec_of "${P_B:-0}") s), from keepers ${P_HOT:-?}"
}

check_set() { # $1 = label, rest = burst labels whose union the replay must return exactly
    local label=$1
    shift
    local expected="$RUN_DIR/expected_$label"
    : > "$expected"
    for b in "$@"; do cat "$RUN_DIR/burst_$b" >> "$expected"; done
    if [ "$P_STATUS" != "CL_SUCCESS" ]; then
        bad "$label: replay status $P_STATUS"
    elif [ "$(sort -n "$P_EVENTS" | uniq -d | wc -l)" -ne 0 ]; then
        bad "$label: replay returned duplicate events"
    elif diff -q <(sort -n "$expected") <(sort -n "$P_EVENTS") > /dev/null; then
        ok "$label: replay returned exactly the $(wc -l < "$expected") events of burst(s) $*"
    else
        bad "$label: replay set differs from burst(s) $*: $(comm -23 <(sort "$expected") <(sort "$P_EVENTS") | wc -l) missing, $(comm -13 <(sort "$expected") <(sort "$P_EVENTS") | wc -l) unexpected"
    fi
}

check_eq() { # $1 = label, $2 = what, $3 = expected, $4 = actual
    if [ "$4" = "$3" ]; then ok "$1: $2 = $3"; else bad "$1: $2 is ${4:-?}, expected $3"; fi
}

# ---------------------------------------------------------------- setup ----
command -v jq > /dev/null || { say "jq not found"; exit 2; }
[ -x "$PROBE" ] || { say "probe not found at $PROBE"; exit 2; }

say "patching installed conf template: keeper ${K_CHUNK}s/${K_ACCEPT}s, grapher ${G_CHUNK}s/${G_ACCEPT}s, report ${REPORT_SECS}s, visibility ${VISIBILITY_SECS}s"
cp "$CONF_TEMPLATE" "$CONF_TEMPLATE.wmark_exact_backup"
jq ".chrono_keeper.DataStoreInternals.story_chunk_duration_secs = $K_CHUNK |
    .chrono_keeper.DataStoreInternals.acceptance_window_secs = $K_ACCEPT |
    .chrono_keeper.DataStoreInternals.inactive_story_delay_secs = 600 |
    .chrono_keeper.DataStoreInternals.watermark_resend_timeout_secs = $RESEND_SECS |
    .chrono_keeper.DataStoreInternals.archive_visibility_delay_secs = $VISIBILITY_SECS |
    .chrono_keeper.DataStoreInternals.shutdown_confirm_timeout_secs = 30 |
    .chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK |
    .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT |
    .chrono_grapher.DataStoreInternals.inactive_story_delay_secs = 600 |
    .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = $REPORT_SECS |
    .chrono_player.ArchiveReaders.archive_window_secs = $G_CHUNK |
    .chrono_player.ArchiveReaders.archive_scan_interval_secs = 2 |
    .chrono_player.Monitoring.monitor.level = \"debug\" |
    .chrono_player.Monitoring.monitor.flushlevel = \"debug\" |
    .chrono_player.Monitoring.monitor.filesize = 104857600" \
    "$CONF_TEMPLATE.wmark_exact_backup" > "$CONF_TEMPLATE.wmark_exact_patched" ||
    { say "conf patch failed; template left as it was"; rm -f "$CONF_TEMPLATE.wmark_exact_backup" "$CONF_TEMPLATE.wmark_exact_patched"; exit 2; }
mv -f "$CONF_TEMPLATE.wmark_exact_patched" "$CONF_TEMPLATE"

restore_conf() { mv -f "$CONF_TEMPLATE.wmark_exact_backup" "$CONF_TEMPLATE"; }
trap 'cleanup; restore_conf; rm -rf "$RUN_DIR"' EXIT

kill_daemons
sleep 2
rm -f "$MONITOR_DIR"/chrono-keeper-*.log "$MONITOR_DIR"/chrono-grapher-*.log "$MONITOR_DIR"/chrono-player-*.log
rm -f "$OUTPUT_DIR"/"$CHRONICLE".*.h5

say "deploying 2 keepers / 1 recording group"
"$DEPLOY" -d -w "$WORK_DIR" -k 2 -r 1 > /dev/null 2>&1 || { say "deploy failed"; exit 2; }
sleep 3
if [ "$(pgrep -c -f "$WORK_DIR/bin/chrono-")" -lt 5 ]; then
    say "expected 5 daemons up"; exit 2
fi
jq ".chrono_client.ClientQueryService.rpc.service_base_port = $PROBE_QUERY_PORT" \
    "$CLIENT_CONF" > "$PROBE_CLIENT_CONF" || { say "probe conf patch failed"; exit 2; }
jq ".chrono_client.ClientQueryService.rpc.service_base_port = $WRITER_QUERY_PORT" \
    "$CLIENT_CONF" > "$WRITER_CLIENT_CONF" || { say "writer conf patch failed"; exit 2; }
: > "$WRITTEN"

# --------------------------------------------------- phase 1: all hot ----
start=$(next_window_start $(($(date +%s) + 1)))
wait_until_sec "$start"
write_burst A 0 || exit 1
sA=$(burst_window A) || exit 1
minA=$(sort -n "$RUN_DIR/burst_A" | head -1)
say "burst A: $BURST events in window [$sA, $((sA + G_CHUNK))); its window is written at $((sA + G_CHUNK + G_ACCEPT))"
wait_until_sec $((sA + PROBE_OFFSET))
probe p1
check_set "phase 1 (hot)" A
check_eq "phase 1 (hot)" "events from the keepers" "$BURST" "$P_HOT"
# No window of the story is written yet, so no keeper has a watermark and B is
# the lowest event the keepers hold (or the story's anchor, if one was reported).
if [ -n "$P_B" ] && [ "$P_B" -ge $((sA * 1000000000)) ] && [ "$P_B" -le "$minA" ]; then
    ok "phase 1 (hot): B is at or below A's first event and inside its window"
else
    bad "phase 1 (hot): B=${P_B:-?}, expected within [$((sA * 1000000000)), $minA]"
fi

# ------------------------------------------------- phase 2: all cold ----
wait_until_sec $((sA + 2 * G_CHUNK + PROBE_OFFSET))
probe p2
check_set "phase 2 (cold)" A
check_eq "phase 2 (cold)" "events from the keepers" 0 "$P_HOT"
check_eq "phase 2 (cold)" "B (end of the empty window after A's)" $(((sA + 2 * G_CHUNK) * 1000000000)) "$P_B"
if [ -f "$OUTPUT_DIR/$CHRONICLE.$STORY.$sA.vlen.h5" ]; then
    ok "phase 2 (cold): A's window is archived as $CHRONICLE.$STORY.$sA.vlen.h5"
else
    bad "phase 2 (cold): no archive file for A's window $sA"
fi

# --------------------------------------------------- phase 3: split ----
sC=$(next_window_start $(($(date +%s) + 1)))
wait_until_sec "$sC"
write_burst C 0 || exit 1
sC=$(burst_window C) || exit 1
minC=$(sort -n "$RUN_DIR/burst_C" | head -1)
say "burst C: $BURST events in window [$sC, $((sC + G_CHUNK)))"
wait_until_sec $((sC + PROBE_OFFSET))
probe p3
check_set "phase 3 (split)" A C
check_eq "phase 3 (split)" "events from the keepers (all of C)" "$BURST" "$P_HOT"
check_eq "phase 3 (split)" "B (start of C's window)" $((sC * 1000000000)) "$P_B"

# --------------------------------------------------- phase 4: frozen ----
# The writer holds the story so the probes can acquire it while the grapher is
# stopped. The grapher is stopped once the keepers have sealed and sent D
# (at most K_CHUNK + K_ACCEPT after its first event, plus a loop pass) and
# before it would write the window ending at D's start, at sD + G_ACCEPT.
sD=$(next_window_start $(($(date +%s) + 1)))
wait_until_sec "$sD"
write_burst D 90 || exit 1
sD=$(burst_window D) || exit 1
say "burst D: $BURST events in window [$sD, $((sD + G_CHUNK))), story held by the writer"
STOP_AT=$((sD + G_ACCEPT - 1)) # the window ending at sD cannot be written before sD + G_ACCEPT
wait_until_sec "$STOP_AT"
pkill -STOP -f "$WORK_DIR/bin/chrono-grapher"
say "grapher stopped at $STOP_AT; W stays at the last window written by then, $((sD - G_CHUNK))"
wait_until_sec $((sD + G_CHUNK + G_ACCEPT + 5)) # well past when D's window would be written
probe p4a
check_set "phase 4 (frozen)" A C D
check_eq "phase 4 (frozen)" "events from the keepers (all of D)" "$BURST" "$P_HOT"
check_eq "phase 4 (frozen)" "B (frozen W)" $(((sD - G_CHUNK) * 1000000000)) "$P_B"
B_frozen=$P_B
sleep 10
probe p4b
check_set "phase 4 (frozen, 10 s later)" A C D
check_eq "phase 4 (frozen, 10 s later)" "B (unchanged)" "$B_frozen" "$P_B"
check_eq "phase 4 (frozen, 10 s later)" "events from the keepers (all of D)" "$BURST" "$P_HOT"

# -------------------------------------------------- phase 5: resumed ----
pkill -CONT -f "$WORK_DIR/bin/chrono-grapher"
resumed=$(date +%s)
say "grapher resumed at $resumed"
# D's window is written on the first loop pass after the resume, reported a
# second later, and served by the archive VISIBILITY_SECS after that
T5=$(($(next_window_start $((resumed + 8))) + PROBE_OFFSET - G_CHUNK))
[ "$T5" -lt $((resumed + 8)) ] && T5=$((T5 + G_CHUNK))
wait_until_sec "$T5"
probe p5
check_set "phase 5 (resumed)" A C D
check_eq "phase 5 (resumed)" "events from the keepers" 0 "$P_HOT"
check_eq "phase 5 (resumed)" "B (caught up)" $(((T5 - G_ACCEPT - 4) * 1000000000)) "$P_B"

# ------------------------------------------------------- archive files ----
expected_files="$CHRONICLE.$STORY.$sA.vlen.h5 $CHRONICLE.$STORY.$sC.vlen.h5 $CHRONICLE.$STORY.$sD.vlen.h5"
actual_files=$(cd "$OUTPUT_DIR" && ls "$CHRONICLE".*.h5 2> /dev/null | sort | tr '\n' ' ' | sed 's/ $//')
check_eq "archive" "files (one per window with events, none rewritten)" "$expected_files" "$actual_files"

kill "$holder_pid" 2> /dev/null
say "-------- $PASS passed, $FAIL failed --------"
[ "$FAIL" -eq 0 ]
