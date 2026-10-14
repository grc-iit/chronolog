#!/usr/bin/env bash
# Checks classify_replay from tests/distributed/nfs/nfs_common.sh on probe
# output written by hand.
set -uo pipefail
repo_root=${1:?repository root required}
source "$repo_root/tests/distributed/nfs/nfs_common.sh" || exit 1
dir=$(mktemp -d)
trap 'rm -rf -- "$dir"' EXIT
failures=0
expect() { # $1 label, $2 expected start of the class line, $3 replay output
    printf '%s\n' "$3" > "$dir/out"
    local got
    got=$(classify_replay "$dir/expected" "$dir/out")
    if [[ "$got" == "$2"* ]]; then echo "PASS: $1"; else echo "FAIL: $1: got '$got'" >&2; failures=$((failures + 1)); fi
}
printf '100\n200\n300\n' > "$dir/expected"
ev() { local t; for t in "$@"; do echo "EVENT $t 7 0"; done; }

expect "all events" "OK missing=0 extra=0 dup=0 status=CL_SUCCESS" "$(ev 300 100 200; echo 'REPLAY_STATUS CL_SUCCESS')"
expect "silent omission" "OMISSION missing=1 extra=0 dup=0" "$(ev 100 200; echo 'REPLAY_STATUS CL_SUCCESS')"
expect "nothing returned" "OMISSION missing=3" "$(echo 'REPLAY_STATUS CL_SUCCESS')"
expect "partial result" "PARTIAL" "$(ev 100; echo 'REPLAY_STATUS CL_ERR_PARTIAL_RESULT')"
expect "error status" "ERROR missing=3 extra=0 dup=0 status=CL_ERR_NOT_EXIST" "$(echo 'REPLAY_STATUS CL_ERR_NOT_EXIST')"
expect "no status line" "ERROR missing=2 extra=0 dup=0 status=none" "$(ev 100)"
expect "duplicate" "DUPLICATE missing=0 extra=0 dup=1" "$(ev 100 200 300 300; echo 'REPLAY_STATUS CL_SUCCESS')"
expect "event of another story" "UNEXPECTED missing=0 extra=1" "$(ev 100 200 300 400; echo 'REPLAY_STATUS CL_SUCCESS')"
expect "extra wins over duplicate" "UNEXPECTED" "$(ev 100 200 300 300 400; echo 'REPLAY_STATUS CL_SUCCESS')"
: > "$dir/empty"
if classify_replay "$dir/empty" "$dir/out" > /dev/null; then
    echo "FAIL: empty expected accepted" >&2
    failures=$((failures + 1))
else
    echo "PASS: empty expected refused"
fi
if classify_replay "$dir/absent" "$dir/out" > /dev/null; then
    echo "FAIL: missing expected file accepted" >&2
    failures=$((failures + 1))
else
    echo "PASS: missing expected file refused"
fi

# timeline_matches on timelines built from classify_replay lines, as the
# visibility test writes them.
line() { # $1 replay output; prints one timeline line
    printf '%s\n' "$1" > "$dir/out"
    echo "  t=5s $(classify_replay "$dir/expected" "$dir/out")"
}
ok_line=$(line "$(ev 100 200 300; echo 'REPLAY_STATUS CL_SUCCESS')")
omission_line=$(line "$(ev 100; echo 'REPLAY_STATUS CL_SUCCESS')")
partial_line=$(line "$(ev 100; echo 'REPLAY_STATUS CL_ERR_PARTIAL_RESULT')")
partial_extra_line=$(line "$(ev 100 400; echo 'REPLAY_STATUS CL_ERR_PARTIAL_RESULT')")
partial_dup_line=$(line "$(ev 100 100; echo 'REPLAY_STATUS CL_ERR_PARTIAL_RESULT')")
unexpected_line=$(line "$(ev 100 200 300 400; echo 'REPLAY_STATUS CL_SUCCESS')")
judge() { # $1 label, $2 expect, $3 0 if the timeline must match, $4... lines
    local label=$1 expect=$2 want=$3
    shift 3
    printf '%s\n' "$@" > "$dir/timeline"
    timeline_matches "$expect" "$dir/timeline"
    local got=$?
    if [ "$got" -eq "$want" ]; then echo "PASS: $label"; else echo "FAIL: $label: returned $got" >&2; failures=$((failures + 1)); fi
}
: > "$dir/timeline"
timeline_matches clean "$dir/timeline"
if [ $? -eq 1 ]; then echo "PASS: clean, no replays"; else echo "FAIL: clean, no replays" >&2; failures=$((failures + 1)); fi
judge "clean, every replay OK" clean 0 "$ok_line" "$ok_line"
judge "clean, one omission" clean 1 "$ok_line" "$omission_line"
judge "clean, one partial result" clean 1 "$ok_line" "$partial_line"
judge "omission, one omission, then complete" omission 0 "$ok_line" "$omission_line" "$ok_line"
judge "omission, partial result missing events" omission 0 "$omission_line" "$partial_line" "$ok_line"
judge "omission, partial result with an extra event" omission 1 "$omission_line" "$partial_extra_line" "$ok_line"
judge "omission, partial result with a duplicate" omission 1 "$omission_line" "$partial_dup_line" "$ok_line"
judge "omission, an unexpected event" omission 1 "$omission_line" "$unexpected_line" "$ok_line"
judge "omission, none missing" omission 1 "$ok_line" "$ok_line"
judge "omission, never complete again" omission 1 "$ok_line" "$omission_line" "$omission_line"
judge "omission, last replay a partial result" omission 1 "$omission_line" "$ok_line" "$partial_line"
judge "unknown expectation" sometimes 2 "$ok_line"

echo "$failures classify checks failed"
test "$failures" -eq 0
