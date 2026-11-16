#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$HOME/chronolog-build/Debug}"
WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
CLIENT_INCLUDE="${WORK_DIR}/include"
scope='Integration_Client(Append|Metadata|Replay)Deadline'

if [[ "${1:-}" == "--help" ]]; then
    cat <<'HELP'
Run the focused client deadline matrix, installed public SDK compile check,
and the real public ReplayStory timeout with the shipped 180000 ms budget.
The replay case takes about three minutes. Requires a deploy-script Debug
build and install in BUILD_DIR and WORK_DIR. Runs serially on both transports.
  BUILD_DIR=... WORK_DIR=... tests/integration/rpc_deadline_matrix_test.sh
HELP
    exit 0
fi
[[ $# -eq 0 ]] || { echo "unexpected argument: $1" >&2; exit 2; }
[[ -f "${BUILD_DIR}/CMakeCache.txt" ]] || { echo "missing build: ${BUILD_DIR}" >&2; exit 2; }
[[ -f "${CLIENT_INCLUDE}/chronolog_client.h" ]] || { echo "missing installed public client header: ${CLIENT_INCLUDE}" >&2; exit 2; }
[[ -f "${CLIENT_INCLUDE}/chronolog_types.h" ]] || { echo "missing installed public types header: ${CLIENT_INCLUDE}" >&2; exit 2; }

if pgrep -af '^([^ ]*/)?chrono-(visor|keeper|grapher|player)( |$)|^([^ ]*/)?chrono-bench( |$)' >&2; then
    echo "conflicting ChronoLog services or benchmark; run serially" >&2
    exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

cat >"${tmp}/sdk-consumer.cpp" <<'CPP'
#include <chronolog_client.h>
int main() { return 0; }
CPP
passed=0
failed=0
if "${CXX:-c++}" -std=c++17 -I"${CLIENT_INCLUDE}" -fsyntax-only "${tmp}/sdk-consumer.cpp"; then
    echo "PASS: standalone installed SDK public-header consumer compiles"
    passed=$((passed + 1))
else
    echo "FAIL: standalone installed SDK public-header consumer does not compile"
    failed=$((failed + 1))
fi

# Runs ctest with the given arguments and adds its counts to passed and
# failed. CTest's own line is "N% tests passed, F tests failed out of T".
run_ctest() {
    local status counts f t
    set +e
    ctest --test-dir "${BUILD_DIR}" -j 1 "$@" --output-on-failure | tee "${tmp}/ctest.log"
    status=${PIPESTATUS[0]}
    set -e
    counts="$(awk '/tests failed out of/ { print $4, $NF }' "${tmp}/ctest.log")"
    if [[ -z "${counts}" ]]; then
        echo "ctest exited ${status} without a result line" >&2
        exit 2
    fi
    read -r f t <<<"${counts}"
    passed=$((passed + t - f))
    failed=$((failed + f))
    [[ "${status}" -eq 0 || "${f}" -gt 0 ]] || failed=$((failed + 1))
}

discovery="$(ctest --test-dir "${BUILD_DIR}" -N -R "${scope}")"
printf '%s\n' "${discovery}"
count="$(awk '/Total Tests:/ { print $3 }' <<<"${discovery}")"
[[ "${count:-0}" -gt 0 ]] || { echo "no client deadline matrix tests discovered" >&2; exit 2; }
grep -q 'ClientAppendDeadline_.*sockets' <<<"${discovery}" || { echo "append sockets cases missing" >&2; exit 2; }
grep -q 'ClientAppendDeadline_.*tcp' <<<"${discovery}" || { echo "append TCP cases missing" >&2; exit 2; }
grep -q 'ClientReplayDeadline_.*sockets' <<<"${discovery}" || { echo "replay sockets cases missing" >&2; exit 2; }
grep -q 'ClientReplayDeadline_.*tcp' <<<"${discovery}" || { echo "replay TCP cases missing" >&2; exit 2; }

run_ctest -R "${scope}" -E shipped-default

# This is a real public Client::ReplayStory call over an actual native RPC to
# a peer that accepts the request but withholds its response. The CTest test
# asserts 175000..185000 ms, the original event vector stays untouched, and no
# acknowledgement or response arrives. Run sockets once; TCP runs in the
# focused short matrix above and the full distributed/NFS matrix separately.
run_ctest -R '^Integration_ClientReplayDeadline_sockets_shipped-default$'

echo "${passed} passed, ${failed} failed"
[[ "${failed}" -eq 0 ]] || exit 1
