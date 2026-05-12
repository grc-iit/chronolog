#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.agent/scripts/mofka_common.sh
source "${SCRIPT_DIR}/mofka_common.sh"

usage() {
  cat <<'USAGE'
Usage: mofka_run_append_smoke.sh [options]

Launch Mofka, run an append-throughput smoke benchmark, collect metrics,
and stop Mofka in one command/session.

Options:
  --result-dir DIR             Existing or new result directory to use.
  --operation-count N          Number of events to append. Default: 50.
  --message-size-bytes N       Payload size. Default: 1024.
  --workflow NAME              append_throughput or append_latency. Default: append_throughput.
  --protocol PROTOCOL          Mercury protocol. Default: ofi+tcp.
  --deployment-mode MODE       local_smoke or bare_metal. Default: local_smoke.
  --node-count N               Node count. Default: 1.
  --slurm-partition NAME       Partition for bare_metal. Default: debug.
  --slurm-nodelist LIST        Optional explicit SLURM nodelist.
  --slurm-time TIME            SLURM time limit. Default: 00:10:00.
  -h, --help                   Show this help.
USAGE
}

RESULT_DIR="${PHASE0_RESULT_DIR:-}"
OPERATION_COUNT="${MOFKA_OPERATION_COUNT:-50}"
MESSAGE_SIZE_BYTES="${MOFKA_MESSAGE_SIZE_BYTES:-1024}"
WORKFLOW="${MOFKA_WORKFLOW:-append_throughput}"
PROTOCOL="${MOFKA_PROTOCOL:-ofi+tcp}"
DEPLOYMENT_MODE="${MOFKA_DEPLOYMENT_MODE:-local_smoke}"
NODE_COUNT="${MOFKA_NODE_COUNT:-1}"
SLURM_PARTITION="${MOFKA_SLURM_PARTITION:-debug}"
SLURM_NODELIST="${MOFKA_SLURM_NODELIST:-}"
SLURM_TIME="${MOFKA_SLURM_TIME:-00:10:00}"

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
    --workflow)
      WORKFLOW="$2"
      shift 2
      ;;
    --protocol)
      PROTOCOL="$2"
      shift 2
      ;;
    --deployment-mode)
      DEPLOYMENT_MODE="$2"
      shift 2
      ;;
    --node-count)
      NODE_COUNT="$2"
      shift 2
      ;;
    --slurm-partition)
      SLURM_PARTITION="$2"
      shift 2
      ;;
    --slurm-nodelist)
      SLURM_NODELIST="$2"
      shift 2
      ;;
    --slurm-time)
      SLURM_TIME="$2"
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
  --deployment-mode "${DEPLOYMENT_MODE}" \
  --node-count "${NODE_COUNT}" \
  --protocol "${PROTOCOL}" \
  --slurm-partition "${SLURM_PARTITION}" \
  --slurm-nodelist "${SLURM_NODELIST}" \
  --slurm-time "${SLURM_TIME}" \
  --wait-seconds 20

mofka_export_spack_runtime_env

bedrock-query "${PROTOCOL}" -f "${MOFKA_RESULT_DIR}/mofka.json" -p \
  > "${MOFKA_RESULT_DIR}/bedrock-query-before-benchmark.json"

python3 "${SCRIPT_DIR}/mofka_append_benchmark.py" \
  --group-file "${MOFKA_RESULT_DIR}/mofka.json" \
  --result-dir "${RESULT_DIR}" \
  --operation-count "${OPERATION_COUNT}" \
  --message-size-bytes "${MESSAGE_SIZE_BYTES}" \
  --node-count "${NODE_COUNT}" \
  --client-count 1 \
  --deployment-mode "${DEPLOYMENT_MODE}" \
  --workflow "${WORKFLOW}" \
  > "${MOFKA_RESULT_DIR}/append-benchmark.stdout.log" \
  2> "${MOFKA_RESULT_DIR}/append-benchmark.stderr.log"

cat > "${RESULT_DIR}/summary.md" <<EOF
# Mofka Append Smoke

- system: Mofka
- workflow: ${WORKFLOW}
- deployment_mode: ${DEPLOYMENT_MODE}
- protocol: ${PROTOCOL}
- partition_type: memory
- node_count: ${NODE_COUNT}
- operation_count: ${OPERATION_COUNT}
- message_size_bytes: ${MESSAGE_SIZE_BYTES}
- metrics: mofka/metrics.json

This smoke validates the Mofka append path and metrics plumbing. A bare_metal run contributes distributed evidence; a local_smoke run does not satisfy the final distributed benchmark requirement.
EOF
