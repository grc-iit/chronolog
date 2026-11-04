#!/usr/bin/env bash
# Native communication fault fixtures. This does not deploy ChronoLog daemons.
# Run replay_fanout_timing_test.sh separately for the standard daemon regression.
set -euo pipefail
if [[ "${1:-}" == "--help" ]]; then
    cat <<'HELP'
Run actual native bulk ownership/fault fixtures on ofi+sockets AND ofi+tcp.
Requires a deploy-script build; no build/install is performed here.
  BUILD_DIR=$HOME/chronolog-build/bounded-rpc-monotonic/Debug \
    tests/integration/rpc_bulk_timeout_test.sh
Fixtures use loopback peers, real registrations/pulls, finite admission,
late receipt/content checks, query response handoff and actual client owners.
Harness peer termination is cleanup only, never production settlement proof.
This script does not replace the separate standard-config daemon tests.
HELP
    exit 0
fi
[[ $# -eq 0 ]] || { echo "unexpected argument" >&2; exit 2; }
BUILD_DIR="${BUILD_DIR:-$HOME/chronolog-build/Debug}"
[[ -f "$BUILD_DIR/CMakeCache.txt" ]] || { echo "missing build: $BUILD_DIR" >&2; exit 2; }
# Invoke on the host (not a process-isolated shell). Other native/distributed
# deployments and benchmarks share timing resources and must finish first.
if pgrep -af '^([^ ]*/)?chrono-(visor|keeper|grapher|player)( |$)|^([^ ]*/)?chrono-bench( |$)' >&2; then
    echo "conflicting ChronoLog processes; run serially" >&2
    exit 2
fi
scope='^Communication_Rpc(BulkLifetime|BulkReceiver|PlayerResponse|ClientBulk)_'
discovery=$(ctest --test-dir "$BUILD_DIR" -N -R "$scope")
printf '%s\n' "$discovery"
count=$(awk '/Total Tests:/ { print $3 }' <<<"$discovery")
[[ "${count:-0}" -gt 0 ]] || { echo "no bulk fixtures discovered" >&2; exit 2; }
for transport in sockets tcp; do
    grep -q "_$transport" <<<"$discovery" || { echo "missing mandatory $transport cases" >&2; exit 2; }
done
ctest --test-dir "$BUILD_DIR" -j 1 -R "$scope" --output-on-failure
