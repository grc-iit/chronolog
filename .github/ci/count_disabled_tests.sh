#!/usr/bin/env bash
# Prints the number of disabled tests under tests/contract: DISABLED_ name prefixes.
# A GTEST_SKIP there is a harness-capability skip (ARCHITECTURE.md section 15), not a disabled test.
# With an allowlist file as the second argument, exits 1 when the count is above the number in it.
set -euo pipefail
root="${1:-tests/contract}"
allowlist="${2:-}"
count=0
if [[ -d "$root" ]]; then
    count=$(grep -rEho --include='*.cpp' --include='*.cc' --include='*.h' --include='*.hpp' \
        'DISABLED_[A-Za-z0-9_]+' "$root" | wc -l || true)
fi
echo "$count"
if [[ -n "$allowlist" ]]; then
    allowed=$(tr -d '[:space:]' < "$allowlist")
    if (( count > allowed )); then
        echo "$root has $count disabled tests, allowlist is $allowed" >&2
        exit 1
    fi
fi
