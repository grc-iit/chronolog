#!/usr/bin/env bash
# Prints the number of disabled tests under tests/contract: DISABLED_ name prefixes.
# A GTEST_SKIP there is a harness-capability skip (ARCHITECTURE.md section 15), not a disabled test.
set -euo pipefail
root="${1:-tests/contract}"
if [[ ! -d "$root" ]]; then
    echo 0
    exit 0
fi
grep -rEho --include='*.cpp' --include='*.cc' --include='*.h' --include='*.hpp' \
    'DISABLED_[A-Za-z0-9_]+' "$root" | wc -l
