#!/bin/bash
# Test-only real daemon regression; never a production stop entry.
#
# With arguments, runs rpc_lifecycle_deadline_test.py with them, for example
#   rpc_lifecycle_deadline_test.sh --protocol ofi+tcp --logs /tmp/x --managed-orderly
#
# With no arguments, as run_integration_tests.sh starts it, runs the default
# case (a paused keeper and a public tail read) once per protocol in
# PROTOCOLS (default "ofi+sockets ofi+tcp"), keeps each run's logs under
# LOG_ROOT (default a new directory under ${TMPDIR:-/tmp}), prints
# "N passed, M failed" and exits 1 when any run failed.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        [PROTOCOLS="ofi+sockets ofi+tcp"] [LOG_ROOT=<dir>] \
#        tests/integration/rpc_lifecycle_deadline_test.sh [fixture arguments]
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
export WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
export BUILD_DIR="${BUILD_DIR:-$HOME/chronolog-build/Debug}"
if [ "$#" -gt 0 ]; then
    exec python3 "$HERE/rpc_lifecycle_deadline_test.py" "$@"
fi

PROTOCOLS="${PROTOCOLS:-ofi+sockets ofi+tcp}"
LOG_ROOT="${LOG_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}/rpc_lifecycle_deadline.XXXXXX")}"
mkdir -p "$LOG_ROOT"
echo "logs: $LOG_ROOT"
pass=0
fail=0
for proto in $PROTOCOLS; do
    # The fixture refuses a logs directory that already exists.
    logs="$LOG_ROOT/${proto//+/-}"
    if python3 "$HERE/rpc_lifecycle_deadline_test.py" --protocol "$proto" --logs "$logs"; then
        echo "PASS: $proto paused keeper public tail"
        pass=$((pass + 1))
    else
        echo "FAIL: $proto paused keeper public tail (logs in $logs)"
        fail=$((fail + 1))
    fi
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
