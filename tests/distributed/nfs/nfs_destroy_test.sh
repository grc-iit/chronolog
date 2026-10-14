#!/usr/bin/env bash
# Destroy test of the NFS pass (#705): writes events A, waits until they are
# archived and the keepers have freed them, destroys the story, creates it
# again under the same name with events B, and checks that a replay from
# another host returns exactly B, that none of A's files is left in the story
# directory while the directory itself stays (the same directory, not one
# created again for B), and that the manifest holds a deletion record for the
# story.
#
# Usage: nfs_destroy_test.sh --work-dir W --probe P --writer HOST --reader HOST
#            --archive DIR --chronicle C --story S [--events 20]
#            [--cache-secs 60] [--log-dir D]
# HOST is an ssh destination, or "local". DIR is the archive as seen from the
# host running this script. C and S must match [a-z0-9_]+, so that their
# directory names are written as typed, and the story must have no events in
# the archive yet. Exit 0 when every check passes, 1 when one fails, 2 on bad
# usage.
set -uo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/nfs_common.sh"
usage() {
    sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

WORK_DIR="" PROBE="" WRITER="" READER="" ARCHIVE="" CHRONICLE="" STORY=""
EVENTS=20 CACHE_SECS=60 LOG_DIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --work-dir) WORK_DIR=${2-}; shift 2 || usage ;;
        --probe) PROBE=${2-}; shift 2 || usage ;;
        --writer) WRITER=${2-}; shift 2 || usage ;;
        --reader) READER=${2-}; shift 2 || usage ;;
        --archive) ARCHIVE=${2-}; shift 2 || usage ;;
        --chronicle) CHRONICLE=${2-}; shift 2 || usage ;;
        --story) STORY=${2-}; shift 2 || usage ;;
        --events) EVENTS=${2-}; shift 2 || usage ;;
        --cache-secs) CACHE_SECS=${2-}; shift 2 || usage ;;
        --log-dir) LOG_DIR=${2-}; shift 2 || usage ;;
        *) usage ;;
    esac
done
for v in WORK_DIR PROBE WRITER READER ARCHIVE CHRONICLE STORY; do [ -n "${!v}" ] || usage; done
[[ "$CHRONICLE" =~ ^[a-z0-9_]+$ && "$STORY" =~ ^[a-z0-9_]+$ ]] || usage
[[ "$EVENTS" =~ ^[1-9][0-9]*$ && "$CACHE_SECS" =~ ^[0-9]+$ ]] || usage
CLIENT_CONF="$WORK_DIR/conf/default-chrono-client-conf.json"
LOG_DIR=${LOG_DIR:-$(mktemp -d)}
mkdir -p "$LOG_DIR"
story_dir="$ARCHIVE/$CHRONICLE/$STORY"

say() { echo "[destroy $CHRONICLE] $*"; }
fail() {
    say "$*"
    echo "DESTROY failed $*"
    exit 1
}

# archived and freed: grapher window and acceptance window, the keepers'
# visibility delay, then the NFS cache time and a margin
wait_secs=$(($(conf_value .chrono_grapher.DataStoreInternals.story_chunk_duration_secs 30) +
    $(conf_value .chrono_grapher.DataStoreInternals.acceptance_window_secs 60) +
    $(conf_value .chrono_keeper.DataStoreInternals.archive_visibility_delay_secs 10) + CACHE_SECS + 30))

write_set() { # $1 label: writes EVENTS events, leaves their timestamps in $LOG_DIR/$1
    probe_on "$WRITER" write "$CHRONICLE" "$STORY" "$EVENTS" > "$LOG_DIR/write_$1.out" 2>&1
    grep -q '^WRITE_STATUS CL_SUCCESS' "$LOG_DIR/write_$1.out" ||
        fail "write $1 on $WRITER: $(grep STATUS "$LOG_DIR/write_$1.out" | tail -1)"
    awk '$1 == "WROTE" { print $2 }' "$LOG_DIR/write_$1.out" > "$LOG_DIR/$1"
    [ "$(wc -l < "$LOG_DIR/$1")" -eq "$EVENTS" ] || fail "write $1 printed $(wc -l < "$LOG_DIR/$1") timestamps"
}
replay_is() { # $1 label: a replay on READER must return exactly the events of $1
    probe_on "$READER" replay "$CHRONICLE" "$STORY" > "$LOG_DIR/replay_$1.out" 2>&1
    local r
    r=$(classify_replay "$LOG_DIR/$1" "$LOG_DIR/replay_$1.out")
    say "replay after $1: $r"
    [[ "$r" == OK* ]]
}

write_set A
say "wrote $EVENTS events A on $WRITER; waiting ${wait_secs}s for them to be archived and freed"
sleep "$wait_secs"
replay_is A || fail "events A were not replayed exactly before the destroy"
ls -1 "$story_dir" 2> /dev/null | grep '\.vlen\.h5$' > "$LOG_DIR/files_A" || true
[ -s "$LOG_DIR/files_A" ] || fail "no archive file for A in $story_dir"
# A destroy keeps the chronicle and story directories (ArchiveLayout.h): on
# NFS a path removed and created again can stay hidden for up to acdirmax.
# Writing B would create a removed directory again, so checking that it exists
# afterwards proves nothing. A destroy deletes only archive files, so a marker
# file of this test's own outlives it exactly when the directory does. The
# marker goes on exit; archive_cleanup.sh leaves a directory with it in place.
marker="$story_dir/.nfs-destroy-test-marker"
trap 'rm -f "$marker"' EXIT
: > "$marker" || fail "cannot create $marker"

probe_on "$WRITER" destroy "$CHRONICLE" "$STORY" > "$LOG_DIR/destroy.out" 2>&1
grep -q '^DESTROY_STATUS CL_SUCCESS' "$LOG_DIR/destroy.out" ||
    fail "destroy on $WRITER: $(grep STATUS "$LOG_DIR/destroy.out" | tail -1)"

write_set B
say "destroyed the story, wrote $EVENTS events B; waiting ${wait_secs}s"
sleep "$wait_secs"
replay_is B || fail "the story created again does not replay exactly events B"
[ -e "$marker" ] || fail "the destroy removed the story directory (B created it again); a destroy keeps it"
while read -r f; do
    [ ! -e "$story_dir/$f" ] || fail "file $f of the destroyed story is still there"
done < "$LOG_DIR/files_A"
jq -s --arg c "$CHRONICLE" --arg s "$STORY" -e \
    'any(.[]; .op == "delete" and .chronicle == $c and (.story == $s or .whole_chronicle == true))' \
    "$ARCHIVE"/%manifest/*.log > /dev/null || fail "no deletion record for $CHRONICLE/$STORY in the manifest"
say "A's $(wc -l < "$LOG_DIR/files_A") files are gone, the directory stays, the manifest holds the deletion"
echo "DESTROY passed"
