#!/usr/bin/env bash
# Prints the number of disabled or skipped tests under tests/contract.
# Counts DISABLED_ name prefixes and GTEST_SKIP uses.
set -euo pipefail
root="${1:-tests/contract}"
if [[ ! -d "$root" ]]; then
    echo 0
    exit 0
fi
grep -rEho --include='*.cpp' --include='*.cc' --include='*.h' --include='*.hpp' \
    'DISABLED_[A-Za-z0-9_]+|GTEST_SKIP[[:space:]]*\(' "$root" | wc -l
