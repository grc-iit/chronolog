#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.agent/scripts/mofka_common.sh
source "${SCRIPT_DIR}/mofka_common.sh"

usage() {
  cat <<'USAGE'
Usage: mofka_run_append_smoke.sh [options]

Launch Mofka, run a local append-throughput smoke benchmark, collect metrics,
and stop Mofka in one command/session.

Options:
  --result-dir DIR             Existing or new result directory to use.
  --operation-count N          Number of events to append. Default: 50.
  --message-size-bytes N       Payload size. Default: 1024.
  --protocol PROTOCOL          Mercury protocol. Default: ofi+tcp.
  -h, --help                   Show this help.
USAGE
}

RESULT_DIR="${PHASE0_RESULT_DIR:-}"
OPERATION_COUNT="${MOFKA_OPERATION_COUNT:-50}"
MESSAGE_SIZE_BYTES="${MOFKA_MESSAGE_SIZE_BYTES:-1024}"
PROTOCOL="${MOFKA_PROTOCOL:-ofi+tcp}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --result-dir)
      RESULT_DIR="$2"
      shift 2
      ;;
    --operation-count)
      OPERATION_COUNT="$2"
      shift 2
      ;;
    --message-size-bytes)
      MESSAGE_SIZE_BYTES="$2"
      shift 2
      ;;
    --protocol)
      PROTOCOL="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

RESULT_DIR="$(phase0_make_result_dir "${RESULT_DIR}")"
MOFKA_RESULT_DIR="${RESULT_DIR}/mofka"
mkdir -p "${MOFKA_RESULT_DIR}"

cleanup() {
  set +e
  if [[ -f "${MOFKA_RESULT_DIR}/mofka.json" ]]; then
    mofka_export_spack_runtime_env
    bedrock-shutdown "${PROTOCOL}" -f "${MOFKA_RESULT_DIR}/mofka.json" \
      > "${MOFKA_RESULT_DIR}/bedrock-shutdown.log" 2>&1
  fi
  "${SCRIPT_DIR}/mofka_stop.sh" --result-dir "${RESULT_DIR}" >/dev/null 2>&1
}
trap cleanup EXIT

"${SCRIPT_DIR}/mofka_launch.sh" \
  --result-dir "${RESULT_DIR}" \
  --deployment-mode local_smoke \
  --node-count 1 \
  --protocol "${PROTOCOL}" \
  --wait-seconds 20

mofka_export_spack_runtime_env

bedrock-query "${PROTOCOL}" -f "${MOFKA_RESULT_DIR}/mofka.json" -p \
  > "${MOFKA_RESULT_DIR}/bedrock-query-before-benchmark.json"

python3 "${SCRIPT_DIR}/mofka_append_benchmark.py" \
  --group-file "${MOFKA_RESULT_DIR}/mofka.json" \
  --result-dir "${RESULT_DIR}" \
  --operation-count "${OPERATION_COUNT}" \
  --message-size-bytes "${MESSAGE_SIZE_BYTES}" \
  --node-count 1 \
  --client-count 1 \
  > "${MOFKA_RESULT_DIR}/append-benchmark.stdout.log" \
  2> "${MOFKA_RESULT_DIR}/append-benchmark.stderr.log"

cat > "${RESULT_DIR}/summary.md" <<EOF
# Mofka Append Smoke

- system: Mofka
- workflow: append_throughput
- deployment_mode: local_smoke
- protocol: ${PROTOCOL}
- partition_type: memory
- operation_count: ${OPERATION_COUNT}
- message_size_bytes: ${MESSAGE_SIZE_BYTES}
- metrics: mofka/metrics.json

This local smoke validates the Mofka append path and metrics plumbing. It does not satisfy the final distributed bare-metal benchmark requirement.
EOF
