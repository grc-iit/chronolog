#!/bin/bash
# Integration test for the player's parallel hot fetch.
#
# A replay asks every keeper of the story for the events it still holds, each
# fetch carrying a 5 s deadline. Those fetches are issued together and waited on
# afterwards (PlaybackService::story_playback_request), so K keepers that never
# answer cost one deadline, not K of them. Nothing else distinguishes the two
# shapes: the events, the logs and the status are identical either way, and only
# the elapsed time tells them apart.
#
# The keepers here are frozen with SIGSTOP rather than stopped. A stopped keeper
# closes its socket and the fetch fails at once, never reaching the deadline, so
# it would measure nothing. A frozen one keeps its endpoint bound and answers
# nothing, which is the case the fan-out exists for.
#
# The probe therefore acquires the story BEFORE the keepers are frozen and waits
# on a file for the go-ahead: acquiring reaches the keepers through the visor, so
# a probe started after the freeze blocks in AcquireStory and never reaches the
# replay at all (REPLAY_SPLIT_PAUSE_FILE in its environment). For the same
# reason the keepers are resumed as soon as the replay has been timed, while the
# probe still has its release to do.
#
#   control : both keepers running, replay is prompt
#   frozen  : both keepers SIGSTOPped, replay takes about one deadline
#
# A sequential implementation would take about two.
#
# Requires an installed tree (deploy_local.sh layout) and the build tree for the
# probe/example binaries. Patches only the *installed* conf template.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        ./replay_fanout_timing_test.sh

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

KEEPERS=2
# KeeperHotFetchClient::kDefaultFetchDeadline
DEADLINE_MS=5000
# one deadline plus room for connect, acquire and the archive read
FANOUT_CEILING_MS=$((DEADLINE_MS + 3000))
# below this the fetches cannot have reached their deadline at all, so the run
# proves nothing about the fan-out
FANOUT_FLOOR_MS=$((DEADLINE_MS - 1500))
# the control must be nowhere near a deadline
CONTROL_CEILING_MS=3000

G_CHUNK_SECS=10
G_ACCEPT_SECS=20
REPORT_SECS=1
VISIBILITY_SECS=5

PASS=0
FAIL=0

say() { echo -e "[replay_fanout_timing_test] $*"; }
ok()  { say "PASS: $*"; PASS=$((PASS + 1)); }
bad() { say "FAIL: $*"; FAIL=$((FAIL + 1)); }

keeper_pids() { pgrep -f "$WORK_DIR/bin/chrono-keeper" || true; }

resume_keepers() {
    local pids
    pids=$(keeper_pids)
    [ -n "$pids" ] && kill -CONT $pids 2> /dev/null
    return 0
}

kill_daemons() {
    # a frozen process never handles SIGTERM, so resume before anything else
    resume_keepers
    sleep 1
    for bin in chrono-visor chrono-keeper chrono-grapher chrono-player; do
        pkill -9 -f "$WORK_DIR/bin/$bin" 2> /dev/null
    done
}

cleanup() {
    resume_keepers
    "$DEPLOY" -s -w "$WORK_DIR" > /dev/null 2>&1
    kill_daemons
}

PROBE_QUERY_PORT="${PROBE_QUERY_PORT:-5562}"
PROBE_CLIENT_CONF="/tmp/fanout_probe_client_conf.json"
RUN_DIR=$(mktemp -d /tmp/fanout_timing.XXXXXX)

replay_ms() { # runs the probe, echoes the milliseconds ReplayStory took (or -1)
    local raw_out="${1:-/dev/null}"
    local out
    out=$("$REPLAY_CHECK" --config "$PROBE_CLIENT_CONF" TailChronicle TailStory 2>&1)
    echo "$out" > "$raw_out"
    local ms
    ms=$(echo "$out" | sed -n 's/^REPLAY_MS \([0-9]*\)$/\1/p')
    echo "${ms:--1}"
}

wait_for_line() { # wait_for_line <file> <pattern> <seconds>
    local file="$1" pattern="$2" limit="$3"
    for _ in $(seq 1 $((limit * 5))); do
        grep -q "$pattern" "$file" 2> /dev/null && return 0
        sleep 0.2
    done
    return 1
}

# ---------------------------------------------------------------- setup ----
command -v jq > /dev/null || { say "jq not found"; exit 2; }
[ -x "$TAIL_EXAMPLE" ] || { say "tail-reader example not found at $TAIL_EXAMPLE"; exit 2; }
[ -x "$REPLAY_CHECK" ] || { say "replay probe not found at $REPLAY_CHECK"; exit 2; }

say "patching installed conf template (grapher ${G_CHUNK_SECS}s/${G_ACCEPT_SECS}s windows)"
cp "$CONF_TEMPLATE" "$CONF_TEMPLATE.fanout_backup"
jq ".chrono_keeper.DataStoreInternals.archive_visibility_delay_secs = $VISIBILITY_SECS |
    .chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK_SECS |
    .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT_SECS |
    .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = $REPORT_SECS" \
    "$CONF_TEMPLATE.fanout_backup" > "$CONF_TEMPLATE" || { say "conf patch failed"; exit 2; }

restore_conf() { mv -f "$CONF_TEMPLATE.fanout_backup" "$CONF_TEMPLATE"; }
trap 'cleanup; restore_conf; rm -rf "$RUN_DIR"' EXIT

kill_daemons
sleep 2
rm -f "$MONITOR_DIR"/chrono-keeper-*.log "$MONITOR_DIR"/chrono-grapher-*.log "$MONITOR_DIR"/chrono-player-*.log
rm -f "$OUTPUT_DIR"/TailChronicle.*.h5

say "deploying $KEEPERS keepers / 1 recording group"
"$DEPLOY" -d -w "$WORK_DIR" -k "$KEEPERS" -r 1 > /dev/null 2>&1 || { say "deploy failed"; exit 2; }
sleep 3
if [ "$(pgrep -c -f "$WORK_DIR/bin/chrono-")" -lt 5 ]; then
    say "expected 5 daemons up"; exit 2
fi

jq ".chrono_client.ClientQueryService.rpc.service_base_port = $PROBE_QUERY_PORT" \
    "$CLIENT_CONF" > "$PROBE_CLIENT_CONF" || { say "probe conf patch failed"; exit 2; }

# ------------------------------------------------------------- writer ----
# 30 events striped over both keepers, then the story is held acquired while we
# probe, so the events are still in keeper memory and the fetch has work to do.
WRITER_OUT="$RUN_DIR/writer.out"
say "writer: 30 events over $KEEPERS keepers (tail-reader example)"
: > "$WRITER_OUT"
"$TAIL_EXAMPLE" --config "$CLIENT_CONF" > "$WRITER_OUT" 2>&1 &
writer_pid=$!
for _ in $(seq 1 30); do
    grep -q "Holding the story acquired" "$WRITER_OUT" && break
    sleep 1
done
say "writer holding; waiting for the keeper seal"
sleep 28

# ------------------------------------------------- control: keepers up ----
# Also populates the player's per-keeper hot-fetch client cache, so the frozen
# run measures the fetch itself rather than a first-time endpoint lookup.
control_ms=$(replay_ms "$RUN_DIR/control.out")
say "control replay took ${control_ms} ms"
if [ "$control_ms" -lt 0 ]; then
    say "control raw output: $(tr '\n' '|' < "$RUN_DIR/control.out")"
    bad "control: the probe did not report REPLAY_MS"
elif [ "$control_ms" -lt "$CONTROL_CEILING_MS" ]; then
    ok "control: replay with both keepers answering took ${control_ms} ms (< ${CONTROL_CEILING_MS})"
else
    bad "control: replay took ${control_ms} ms with keepers up; the box is too slow to time a fan-out"
fi

# --------------------------------------------------- frozen keepers ----
FROZEN_OUT="$RUN_DIR/frozen.out"
GO_FILE="$RUN_DIR/go"
rm -f "$GO_FILE"

# acquire first: AcquireStory reaches the keepers through the visor, so it has
# to complete while they still answer
: > "$FROZEN_OUT"
REPLAY_SPLIT_PAUSE_FILE="$GO_FILE" "$REPLAY_CHECK" --config "$PROBE_CLIENT_CONF" TailChronicle TailStory > "$FROZEN_OUT" 2>&1 &
probe_pid=$!
if ! wait_for_line "$FROZEN_OUT" "^ACQUIRED$" 60; then
    say "probe raw output: $(tr '\n' '|' < "$FROZEN_OUT")"
    say "the probe never acquired the story"; exit 2
fi

pids=$(keeper_pids)
count=$(echo "$pids" | wc -w)
if [ "$count" -ne "$KEEPERS" ]; then
    say "expected $KEEPERS keeper processes, found $count"; exit 2
fi
say "probe holds the story; freezing both keepers with SIGSTOP (pids: $(echo $pids | tr '\n' ' '))"
kill -STOP $pids || { say "SIGSTOP failed"; exit 2; }
sleep 1

say "releasing the probe into its replay"
: > "$GO_FILE"

# resume as soon as the replay is timed: the probe's ReleaseStory reaches the
# keepers too, and would block on frozen ones
if wait_for_line "$FROZEN_OUT" "^REPLAY_MS " 120; then
    say "replay timed; resuming both keepers"
else
    say "no REPLAY_MS within 120 s; resuming both keepers anyway"
fi
resume_keepers
wait "$probe_pid" 2> /dev/null
frozen_ms=$(sed -n 's/^REPLAY_MS \([0-9]*\)$/\1/p' "$FROZEN_OUT" | head -1)
frozen_ms=${frozen_ms:--1}
say "frozen replay took ${frozen_ms} ms"
sleep 2

if [ "$frozen_ms" -lt 0 ]; then
    say "frozen raw output: $(tr '\n' '|' < "$RUN_DIR/frozen.out")"
    bad "frozen: the probe did not report REPLAY_MS"
else
    if [ "$frozen_ms" -ge "$FANOUT_FLOOR_MS" ]; then
        ok "frozen: the fetches reached their deadline (${frozen_ms} ms >= ${FANOUT_FLOOR_MS})"
    else
        bad "frozen: replay returned in ${frozen_ms} ms, below one deadline — the fetches failed fast, so this run says nothing about the fan-out"
    fi
    if [ "$frozen_ms" -lt "$FANOUT_CEILING_MS" ]; then
        ok "frozen: $KEEPERS silent keepers cost ${frozen_ms} ms, about one deadline (< ${FANOUT_CEILING_MS})"
    else
        bad "frozen: $KEEPERS silent keepers cost ${frozen_ms} ms; a fan-out should cost about one ${DEADLINE_MS} ms deadline, not $KEEPERS"
    fi
fi

# the replay must still answer, and say it was short
status=$(sed -n 's/^REPLAY_STATUS \(.*\)$/\1/p' "$RUN_DIR/frozen.out" | head -1)
if [ "$status" = "CL_ERR_PARTIAL_RESULT" ]; then
    ok "frozen: replay reported CL_ERR_PARTIAL_RESULT rather than a silent short answer"
else
    bad "frozen: expected CL_ERR_PARTIAL_RESULT with both keepers silent, got '${status:-none}'"
fi

wait "$writer_pid" 2> /dev/null

# ---------------------------------------------------------------- done ----
say "-------- $PASS passed, $FAIL failed --------"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
