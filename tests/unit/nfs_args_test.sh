#!/usr/bin/env bash
# Checks that the NFS pass scripts refuse an option whose value is missing.
# Every option that takes a value is given last, with nothing after it; each
# script must exit 2 (usage) within a few seconds instead of looping on the
# same argument.
set -uo pipefail
repo_root=${1:?repository root required}
nfs="$repo_root/tests/distributed/nfs"
failures=0 checked=0
for script in nfs_pass.sh nfs_visibility_test.sh nfs_destroy_test.sh; do
    # the options that take a value: the case branches that shift 2
    mapfile -t opts < <(grep -oE '^ +--[a-z-]+\) .*shift 2' "$nfs/$script" | grep -oE -- '--[a-z-]+' | sort -u)
    if [ ${#opts[@]} -eq 0 ]; then
        echo "FAIL: $script: found no value options to check" >&2
        failures=$((failures + 1))
        continue
    fi
    for opt in "${opts[@]}"; do
        checked=$((checked + 1))
        timeout 10 "$nfs/$script" "$opt" > /dev/null 2>&1
        rc=$?
        if [ "$rc" -eq 2 ]; then
            echo "PASS: $script $opt"
        else
            echo "FAIL: $script $opt with no value exited $rc (124 is the timeout)" >&2
            failures=$((failures + 1))
        fi
    done
done
echo "$failures of $checked missing-value checks failed"
test "$failures" -eq 0
