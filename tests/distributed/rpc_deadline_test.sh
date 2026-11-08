#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PIPELINE="${REPO_ROOT}/docker/local_distributed_ci.sh"

IMAGE=""
IFACE="eth0"
SELF_CHECK=0
VERIFY_LOG=""
VERIFY_PROTOCOL=""

verify_completed_log() {
    python3 - "$1" "$2" "$3" <<'PYVERIFY'
import json
from pathlib import Path
import sys

text = Path(sys.argv[1]).read_text()
protocol, interface = sys.argv[2:]
if protocol not in ("ofi+sockets", "ofi+tcp"):
    raise SystemExit("unsupported audit protocol")
for stage in ("6", "7", "8", "9", "10", "11", "11b", "12"):
    if f"✅ {stage}. " not in text:
        raise SystemExit(f"missing completed stage {stage}")
required = f"Generated group and component configs use {protocol}; keeper/grapher extraction fields match"
if required not in text:
    raise SystemExit("missing generated daemon/extraction config audit")
for marker in ("RPC service protocols:", "Keeper/Grapher extraction protocols:"):
    try:
        value, _ = json.JSONDecoder().raw_decode(text.split(marker, 1)[1].lstrip())
    except (IndexError, ValueError) as error:
        raise SystemExit(f"missing or malformed {marker}") from error
    if value != [protocol]:
        raise SystemExit(f"wrong protocol audit at {marker}: {value}")
if protocol == "ofi+tcp" and f"TCP interface verified: {interface} routes between the actual containers" not in text:
    raise SystemExit("missing reachable TCP interface audit")
PYVERIFY
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        -i) IMAGE="$2"; shift 2 ;;
        --rpc-tcp-iface) IFACE="$2"; shift 2 ;;
        --self-check) SELF_CHECK=1; shift ;;
        --verify-log)
            [[ $# -ge 3 ]] || { echo "--verify-log requires LOG PROTOCOL" >&2; exit 2; }
            VERIFY_LOG="$2"; VERIFY_PROTOCOL="$3"; shift 3 ;;

        -h|--help)
            cat <<'HELP'
Run the distributed ChronoLog pipeline and NFS pass on ofi+sockets and ofi+tcp.
Runs are serial. Each is built from this worktree; --skip-build is not used.
Pass the immutable CI image matching this checkout with -i; TCP iface is eth0.
  tests/distributed/rpc_deadline_test.sh -i IMAGE [--rpc-tcp-iface IFACE]
  tests/distributed/rpc_deadline_test.sh --self-check
  tests/distributed/rpc_deadline_test.sh --verify-log LOG PROTOCOL [--rpc-tcp-iface IFACE]
Logs are saved in B13_LOG_DIR or a new directory under /tmp.
HELP
            exit 0
            ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ -n "${VERIFY_LOG}" ]]; then
    verify_completed_log "${VERIFY_LOG}" "${VERIFY_PROTOCOL}" "${IFACE}"
    echo "PASS: ${VERIFY_PROTOCOL} completed pipeline log audit"
    exit 0
fi

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

    # The pipeline uses pretty-printed jq arrays. Validate the JSON value,
    # and keep incomplete/mixed-protocol logs from becoming audit successes.
    python3 - "${tmp}" <<'PYFIXTURE'
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
for protocol in ("ofi+sockets", "ofi+tcp"):
    text = "\n".join(f"✅ {stage}. completed" for stage in ("6", "7", "8", "9", "10", "11", "11b", "12"))
    text += f"\nGenerated group and component configs use {protocol}; keeper/grapher extraction fields match\n"
    for marker in ("RPC service protocols:", "Keeper/Grapher extraction protocols:"):
        text += marker + "\n" + json.dumps([protocol], indent=2) + "\n"
    text += "TCP interface verified: eth0 routes between the actual containers\n"
    (root / (protocol + ".log")).write_text(text)
    (root / (protocol + ".compact.log")).write_text(text.replace(json.dumps([protocol], indent=2), json.dumps([protocol])))
    (root / (protocol + ".incomplete.log")).write_text(text.replace("✅ 11b. completed", "missing NFS"))
    (root / (protocol + ".mixed.log")).write_text(text.replace(json.dumps([protocol], indent=2), json.dumps(["ofi+sockets", "ofi+tcp"]), 1))
PYFIXTURE
    for protocol in ofi+sockets ofi+tcp; do
        verify_completed_log "${tmp}/${protocol}.log" "${protocol}" eth0
        verify_completed_log "${tmp}/${protocol}.compact.log" "${protocol}" eth0
        for invalid in incomplete mixed; do
            if verify_completed_log "${tmp}/${protocol}.${invalid}.log" "${protocol}" eth0 >"${tmp}/invalid-audit" 2>&1; then
                echo "${invalid} pipeline log must fail verification" >&2
                exit 1
            fi
        done
    done
    echo "PASS: pretty/compact JSON protocol audits accepted; incomplete/mixed logs rejected"

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

if [[ -z "${IMAGE}" ]]; then
    echo "-i IMAGE is required; use the immutable CI image matching this checkout" >&2
    exit 2
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
        if verify_completed_log "${log}" "${protocol}" "${IFACE}"; then
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
