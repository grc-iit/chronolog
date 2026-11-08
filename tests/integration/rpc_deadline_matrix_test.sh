#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-/home/kfeng/chronolog-build/bounded-rpc-monotonic/Debug}"
WORK_DIR="${WORK_DIR:-/home/kfeng/chronolog-install-bounded-rpc-monotonic/chronolog}"
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
"${CXX:-c++}" -std=c++17 -I"${CLIENT_INCLUDE}" -fsyntax-only "${tmp}/sdk-consumer.cpp"
echo "PASS: standalone installed SDK public-header consumer compiles"

discovery="$(ctest --test-dir "${BUILD_DIR}" -N -R "${scope}")"
printf '%s\n' "${discovery}"
count="$(awk '/Total Tests:/ { print $3 }' <<<"${discovery}")"
[[ "${count:-0}" -gt 0 ]] || { echo "no client deadline matrix tests discovered" >&2; exit 2; }
grep -q 'ClientAppendDeadline_.*sockets' <<<"${discovery}" || { echo "append sockets cases missing" >&2; exit 2; }
grep -q 'ClientAppendDeadline_.*tcp' <<<"${discovery}" || { echo "append TCP cases missing" >&2; exit 2; }
grep -q 'ClientReplayDeadline_.*sockets' <<<"${discovery}" || { echo "replay sockets cases missing" >&2; exit 2; }
grep -q 'ClientReplayDeadline_.*tcp' <<<"${discovery}" || { echo "replay TCP cases missing" >&2; exit 2; }

set +e
ctest --test-dir "${BUILD_DIR}" -j 1 -R "${scope}" -E shipped-default --output-on-failure
matrix_status=$?
set -e
echo "Focused client matrix exit status: ${matrix_status}"

# This is a real public Client::ReplayStory call over an actual native RPC to
# a peer that accepts the request but withholds its response. The CTest test
# asserts 175000..185000 ms, the original event vector stays untouched, and no
# acknowledgement or response arrives. Run sockets once; TCP runs in the
# focused short matrix above and the full distributed/NFS matrix separately.
ctest --test-dir "${BUILD_DIR}" -j 1 \
    -R '^Integration_ClientReplayDeadline_sockets_shipped-default$' --output-on-failure

exit "${matrix_status}"
