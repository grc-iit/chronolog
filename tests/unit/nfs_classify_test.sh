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

echo "$failures classify checks failed"
test "$failures" -eq 0
