#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PIPELINE="${REPO_ROOT}/docker/local_distributed_ci.sh"

IMAGE="chronolog-base:spack122-local"
IFACE="eth0"
SELF_CHECK=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        -i) IMAGE="$2"; shift 2 ;;
        --rpc-tcp-iface) IFACE="$2"; shift 2 ;;
        --self-check) SELF_CHECK=1; shift ;;
        -h|--help)
            cat <<'HELP'
Run the distributed ChronoLog pipeline and NFS pass on ofi+sockets and ofi+tcp.
Runs are serial. Each is built from this worktree; --skip-build is not used.
The default image is chronolog-base:spack122-local and the TCP iface is eth0.
  tests/distributed/rpc_deadline_test.sh [-i IMAGE] [--rpc-tcp-iface IFACE]
  tests/distributed/rpc_deadline_test.sh --self-check
Logs are saved in B13_LOG_DIR or a new directory under /tmp.
HELP
            exit 0
            ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ ${SELF_CHECK} -eq 1 ]]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "${tmp}"' EXIT
    set +e
    "${PIPELINE}" --rpc-protocol ofi+rdma >"${tmp}/stdout" 2>"${tmp}/stderr"
    status=$?
    set -e
    if [[ ${status} -ne 2 ]]; then
        echo "unsupported protocol must fail during argument validation with status 2; got ${status}" >&2
        cat "${tmp}/stdout" "${tmp}/stderr" >&2
        exit 1
    fi
    if ! grep -q 'unsupported --rpc-protocol: ofi+rdma' "${tmp}/stderr"; then
        echo "unsupported protocol diagnostic missing" >&2
        cat "${tmp}/stderr" >&2
        exit 1
    fi
    set +e
    "${PIPELINE}" --rpc-protocol ofi+tcp --rpc-tcp-iface lo >"${tmp}/loopback-out" 2>"${tmp}/loopback-err"
    status=$?
    set -e
    if [[ ${status} -ne 2 ]] || ! grep -q 'loopback is not allowed for distributed TCP: lo' "${tmp}/loopback-err"; then
        echo "loopback TCP interface must be rejected before any container work" >&2
        cat "${tmp}/loopback-out" "${tmp}/loopback-err" >&2
        exit 1
    fi

    mkdir -p "${tmp}/fakebin"
    cat >"${tmp}/fakebin/docker" <<'DOCKER'
#!/usr/bin/env bash
if [[ "${1:-}" == compose && "${2:-}" == version ]]; then exit 0; fi
if [[ "${1:-}" == info ]]; then
    : >"${B13_SIGNAL_MARKER}"
    while true; do sleep 1; done
fi
exit 0
DOCKER
    chmod +x "${tmp}/fakebin/docker"
    marker="${tmp}/docker-info-entered"
    set +e
    B13_SIGNAL_MARKER="${marker}" PATH="${tmp}/fakebin:${PATH}" \
        timeout --preserve-status -s INT 2 "${PIPELINE}" -i b13-signal-test \
        >"${tmp}/signal-out" 2>"${tmp}/signal-err"
    status=$?
    set -e
    if [[ ${status} -ne 130 ]]; then
        echo "SIGINT during preflight must exit 130 after cleanup; got ${status}" >&2
        cat "${tmp}/signal-out" "${tmp}/signal-err" >&2
        exit 1
    fi
    echo "PASS: invalid transports and loopback are rejected; SIGINT exits 130"
    exit 0
fi

RUNS_DIR="${B13_LOG_DIR:-$(mktemp -d -t chronolog-b13.XXXXXX)}"
mkdir -p "${RUNS_DIR}"
echo "B13 logs: ${RUNS_DIR}"
failed=0
for protocol in ofi+sockets ofi+tcp; do
    log="${RUNS_DIR}/distributed-${protocol#ofi+}.log"
    echo "Running distributed + NFS pipeline for ${protocol}; log ${log}"
    if "${PIPELINE}" -i "${IMAGE}" --rpc-protocol "${protocol}" \
        --rpc-tcp-iface "${IFACE}" --nfs >"${log}" 2>&1; then
        if grep -q '✅ 6. Verify deployment' "${log}" && \
           grep -q "Generated group and component configs use ${protocol}; keeper/grapher extraction fields match" "${log}" && \
           grep -q '✅ 11b. NFS pass' "${log}" && \
           grep -q '✅ 12. Stop ChronoLog and clean archive' "${log}" && \
           grep -A1 'RPC service protocols:' "${log}" | grep -Fq "[\"${protocol}\"]" && \
           grep -A1 'Keeper/Grapher extraction protocols:' "${log}" | grep -Fq "[\"${protocol}\"]" && \
           { [[ "${protocol}" != ofi+tcp ]] || grep -q "TCP interface verified: ${IFACE} routes between the actual containers" "${log}"; }; then
            echo "PASS: ${protocol} distributed deployment audit + NFS"
        else
            echo "FAIL: ${protocol} pipeline exited zero without deployment audit, NFS, or cleanup evidence; log ${log}" >&2
            failed=1
        fi
    else
        status=$?
        echo "FAIL: ${protocol} distributed pipeline + NFS (exit ${status}); log ${log}" >&2
        failed=1
    fi
done
exit "${failed}"
