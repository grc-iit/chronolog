#!/usr/bin/env bash
# Visibility test of the NFS pass (#705): writes bursts of events from one
# host, then replays the story from another every few seconds until well past
# the keepers freeing its chunks plus the NFS attribute cache time, and
# compares every replay with the events written, by timestamp.
#   --expect clean     every replay returns each event exactly once with CL_SUCCESS
#   --expect omission  at least one replay misses events and still returns
#                      CL_SUCCESS (the nocto negative control), the last
#                      replay returns every event, and no replay returns
#                      an event not written or one twice
#
# Usage: nfs_visibility_test.sh --work-dir W --probe P --writer HOST --reader HOST
#            --chronicle C --story S --expect clean|omission [--bursts 2]
#            [--burst-events 20] [--burst-gap 35] [--interval 5] [--cache-secs 60]
#            [--log-dir D]
# HOST is an ssh destination, or "local". Exit 0 when the outcome matches
# --expect, 1 when it does not, 2 on bad usage. The story must have no events
# in the archive yet: events from an earlier run on the same archive come
# back in every replay and count as extra.
set -uo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/nfs_common.sh"
usage() {
    sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

WORK_DIR="" PROBE="" WRITER="" READER="" CHRONICLE="" STORY="" EXPECT=""
BURSTS=2 BURST_EVENTS=20 BURST_GAP=35 INTERVAL=5 CACHE_SECS=60 LOG_DIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --work-dir) WORK_DIR=${2-}; shift 2 || usage ;;
        --probe) PROBE=${2-}; shift 2 || usage ;;
        --writer) WRITER=${2-}; shift 2 || usage ;;
        --reader) READER=${2-}; shift 2 || usage ;;
        --chronicle) CHRONICLE=${2-}; shift 2 || usage ;;
        --story) STORY=${2-}; shift 2 || usage ;;
        --expect) EXPECT=${2-}; shift 2 || usage ;;
        --bursts) BURSTS=${2-}; shift 2 || usage ;;
        --burst-events) BURST_EVENTS=${2-}; shift 2 || usage ;;
        --burst-gap) BURST_GAP=${2-}; shift 2 || usage ;;
        --interval) INTERVAL=${2-}; shift 2 || usage ;;
        --cache-secs) CACHE_SECS=${2-}; shift 2 || usage ;;
        --log-dir) LOG_DIR=${2-}; shift 2 || usage ;;
        *) usage ;;
    esac
done
for v in WORK_DIR PROBE WRITER READER CHRONICLE STORY; do [ -n "${!v}" ] || usage; done
[[ "$EXPECT" == clean || "$EXPECT" == omission ]] || usage
for v in BURSTS BURST_EVENTS BURST_GAP INTERVAL CACHE_SECS; do [[ "${!v}" =~ ^[0-9]+$ ]] || usage; done
[ "$BURSTS" -gt 0 ] && [ "$BURST_EVENTS" -gt 0 ] || usage
CLIENT_CONF="$WORK_DIR/conf/default-chrono-client-conf.json"
LOG_DIR=${LOG_DIR:-$(mktemp -d)}
mkdir -p "$LOG_DIR"
expected="$LOG_DIR/expected"
: > "$expected"
: > "$LOG_DIR/timeline"

say() { echo "[visibility $CHRONICLE] $*"; }

# Start the first burst 2 s into a grapher window (windows start on multiples
# of their length since the epoch). Its window then closes about a window
# length later, and the grapher appends the file's manifest record after the
# acceptance window: with the template, about 88 s after the burst, so after
# the reader's first replay (about 66 s) and well inside the 60 s its NFS
# client may keep that replay's attributes. Starting at a random point, a
# burst near the end of a window had its record appended before the first
# replay, and about one nocto pass in five saw no omission.
window=$(conf_value .chrono_grapher.DataStoreInternals.story_chunk_duration_secs 30)
now_ns=$(date +%s%N)
wait_ns=$(((2000000000 - now_ns % (window * 1000000000) + window * 1000000000) % (window * 1000000000)))
sleep "$((wait_ns / 1000000000)).$(printf '%09d' $((wait_ns % 1000000000)))"

for b in $(seq 1 "$BURSTS"); do
    out="$LOG_DIR/write_$b.out"
    probe_on "$WRITER" write "$CHRONICLE" "$STORY" "$BURST_EVENTS" > "$out" 2>&1
    if ! grep -q '^WRITE_STATUS CL_SUCCESS' "$out"; then
        say "burst $b on $WRITER failed: $(grep -E 'STATUS' "$out" | tail -1)"
        echo "VISIBILITY failed ok=0 omission=0 other=0"
        exit 1
    fi
    awk '$1 == "WROTE" { print $2 }' "$out" >> "$expected"
    [ "$b" -lt "$BURSTS" ] && sleep "$BURST_GAP"
done
if [ "$(wc -l < "$expected")" -ne $((BURSTS * BURST_EVENTS)) ]; then
    say "the probe printed $(wc -l < "$expected") timestamps for $((BURSTS * BURST_EVENTS)) events"
    echo "VISIBILITY failed ok=0 omission=0 other=0"
    exit 1
fi

# settle: the last burst's keeper chunk is sealed and sent to the grapher.
# watch: the grapher window closes and is archived, the keepers free the
# chunk after their visibility delay, and a reader's NFS cache expires.
settle=$(($(conf_value .chrono_keeper.DataStoreInternals.story_chunk_duration_secs 10) +
    $(conf_value .chrono_keeper.DataStoreInternals.acceptance_window_secs 15) + 5))
watch=$(($(conf_value .chrono_grapher.DataStoreInternals.story_chunk_duration_secs 30) +
    $(conf_value .chrono_grapher.DataStoreInternals.acceptance_window_secs 60) +
    $(conf_value .chrono_keeper.DataStoreInternals.archive_visibility_delay_secs 10) + CACHE_SECS + 30))
say "wrote $(wc -l < "$expected") events on $WRITER; replaying on $READER every ${INTERVAL}s for ${watch}s after ${settle}s"
sleep "$settle"

ok=0 omission=0 other=0 start=$SECONDS k=0
while [ $((SECONDS - start)) -lt "$watch" ]; do
    k=$((k + 1))
    out="$LOG_DIR/replay_$k.out"
    probe_on "$READER" replay "$CHRONICLE" "$STORY" > "$out" 2>&1
    result=$(classify_replay "$expected" "$out")
    echo "  t=$((SECONDS - start))s $result" | tee -a "$LOG_DIR/timeline"
    case "$result" in
        OK*) ok=$((ok + 1)) ;;
        OMISSION*) omission=$((omission + 1)) ;;
        *) other=$((other + 1)) ;;
    esac
    sleep "$INTERVAL"
done

if timeline_matches "$EXPECT" "$LOG_DIR/timeline"; then
    echo "VISIBILITY $EXPECT ok=$ok omission=$omission other=$other"
    exit 0
fi
say "expected $EXPECT; the timeline is in $LOG_DIR/timeline"
echo "VISIBILITY failed ok=$ok omission=$omission other=$other"
exit 1
