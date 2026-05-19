#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.agent/scripts/mofka_common.sh
source "${SCRIPT_DIR}/mofka_common.sh"

reject_tmpfs_storage_path() {
  local label="$1"
  local path="$2"
  local resolved
  resolved="$(realpath -m "${path}")"
  case "${resolved}" in
    /tmp|/tmp/*|/dev/shm|/dev/shm/*)
      echo "${label} must not be tmpfs-backed (${resolved}); Mofka storage benchmarks must not use memory-backed filesystems" >&2
      exit 2
      ;;
  esac
  if command -v findmnt >/dev/null 2>&1; then
    local fs_type
    local mount_probe="${resolved}"
    while [[ ! -e "${mount_probe}" && "${mount_probe}" != "/" ]]; do
      mount_probe="$(dirname "${mount_probe}")"
    done
    fs_type="$(findmnt -T "${mount_probe}" -no FSTYPE 2>/dev/null | head -n 1 || true)"
    case "${fs_type}" in
      tmpfs|ramfs|devtmpfs)
        echo "${label} resolves to ${fs_type} (${resolved}); Mofka storage benchmarks must not use memory-backed filesystems" >&2
        exit 2
        ;;
    esac
  fi
}

usage() {
  cat <<'USAGE'
Usage: mofka_run_append_distributed.sh [options]

Launch Mofka, run an append-throughput benchmark, collect metrics,
and stop Mofka in one command/session.

Options:
  --result-dir DIR             Existing or new result directory to use.
  --operation-count N          Number of events to append. Default: 50.
  --message-size-bytes N       Payload size. Default: 1024.
  --workflow NAME              append_throughput, append_latency, or range_retrieval. Default: append_throughput.
  --partition-type TYPE        memory or default. Default: default.
  --storage-target-type TYPE   Warabi target type: pmdk, abtio, or memory. Default: pmdk.
  --storage-target-size N      Storage target size in bytes for PMDK targets. Default: 67108864.
  --precreate-storage-provider yes|no
                              Pre-create a Warabi provider in the Bedrock
                              storage config. Default: yes.
  --group-ping-timeout-ms N    Centralized Flock group ping timeout. Default: 1000.
  --group-ping-interval-min-ms N
                               Centralized Flock group minimum ping interval. Default: 1000.
  --group-ping-interval-max-ms N
                               Centralized Flock group maximum ping interval. Default: 1000.
  --group-ping-max-timeouts N  Centralized Flock group missed-ping eviction threshold. Default: 3.
  --producer-wait-mode MODE    per_event, after_loop, or none. Default: per_event.
  --producer-flush-mode MODE   after_loop or none. Default: after_loop.
  --protocol PROTOCOL          Mercury protocol. Default: ofi+tcp.
  --deployment-mode MODE       local_validation or bare_metal. Default: local_validation.
  --node-count N               Node count. Default: 1.
  --client-count N             Parallel benchmark client count. Default: 1.
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
PARTITION_TYPE="${MOFKA_PARTITION_TYPE:-default}"
STORAGE_TARGET_TYPE="${MOFKA_STORAGE_TARGET_TYPE:-pmdk}"
STORAGE_TARGET_SIZE="${MOFKA_STORAGE_TARGET_SIZE:-67108864}"
PRECREATE_STORAGE_PROVIDER="${MOFKA_PRECREATE_STORAGE_PROVIDER:-yes}"
GROUP_PING_TIMEOUT_MS="${MOFKA_GROUP_PING_TIMEOUT_MS:-1000}"
GROUP_PING_INTERVAL_MIN_MS="${MOFKA_GROUP_PING_INTERVAL_MIN_MS:-1000}"
GROUP_PING_INTERVAL_MAX_MS="${MOFKA_GROUP_PING_INTERVAL_MAX_MS:-1000}"
GROUP_PING_MAX_TIMEOUTS="${MOFKA_GROUP_PING_MAX_TIMEOUTS:-3}"
PRODUCER_WAIT_MODE="${MOFKA_PRODUCER_WAIT_MODE:-per_event}"
PRODUCER_FLUSH_MODE="${MOFKA_PRODUCER_FLUSH_MODE:-after_loop}"
PROTOCOL="${MOFKA_PROTOCOL:-ofi+tcp}"
DEPLOYMENT_MODE="${MOFKA_DEPLOYMENT_MODE:-local_validation}"
NODE_COUNT="${MOFKA_NODE_COUNT:-1}"
CLIENT_COUNT="${MOFKA_CLIENT_COUNT:-1}"
SLURM_PARTITION="${MOFKA_SLURM_PARTITION:-debug}"
SLURM_NODELIST="${MOFKA_SLURM_NODELIST:-}"
SLURM_TIME="${MOFKA_SLURM_TIME:-00:10:00}"
LAUNCH_WAIT_SECONDS="${MOFKA_LAUNCH_WAIT_SECONDS:-120}"

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
    --partition-type)
      PARTITION_TYPE="$2"
      shift 2
      ;;
    --storage-target-type)
      STORAGE_TARGET_TYPE="$2"
      shift 2
      ;;
    --storage-target-size)
      STORAGE_TARGET_SIZE="$2"
      shift 2
      ;;
    --precreate-storage-provider)
      PRECREATE_STORAGE_PROVIDER="$2"
      shift 2
      ;;
    --group-ping-timeout-ms)
      GROUP_PING_TIMEOUT_MS="$2"
      shift 2
      ;;
    --group-ping-interval-min-ms)
      GROUP_PING_INTERVAL_MIN_MS="$2"
      shift 2
      ;;
    --group-ping-interval-max-ms)
      GROUP_PING_INTERVAL_MAX_MS="$2"
      shift 2
      ;;
    --group-ping-max-timeouts)
      GROUP_PING_MAX_TIMEOUTS="$2"
      shift 2
      ;;
    --producer-wait-mode)
      PRODUCER_WAIT_MODE="$2"
      shift 2
      ;;
    --producer-flush-mode)
      PRODUCER_FLUSH_MODE="$2"
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
    --client-count)
      CLIENT_COUNT="$2"
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

if [[ "${CLIENT_COUNT}" -lt 1 ]]; then
  echo "Mofka client-count must be >= 1" >&2
  exit 2
fi

RESULT_DIR="$(phase0_make_result_dir "${RESULT_DIR}")"
reject_tmpfs_storage_path "--result-dir" "${RESULT_DIR}"
export PHASE0_RESULT_DIR="${RESULT_DIR}"
MOFKA_RESULT_DIR="${RESULT_DIR}/mofka"
mkdir -p "${MOFKA_RESULT_DIR}"

if [[ "${DEPLOYMENT_MODE}" == "bare_metal" && "${NODE_COUNT}" -gt 1 && -z "${SLURM_JOB_ID:-}" && "${MOFKA_INSIDE_ALLOCATION:-0}" != "1" ]]; then
  SBATCH_NODELIST_ARGS=()
  if [[ -n "${SLURM_NODELIST}" ]]; then
    SBATCH_NODELIST_ARGS=(--nodelist="${SLURM_NODELIST}")
  fi
  RECURSIVE_ARGS=(
    --result-dir "${RESULT_DIR}"
    --operation-count "${OPERATION_COUNT}"
    --message-size-bytes "${MESSAGE_SIZE_BYTES}"
    --workflow "${WORKFLOW}"
    --partition-type "${PARTITION_TYPE}"
    --storage-target-type "${STORAGE_TARGET_TYPE}"
    --storage-target-size "${STORAGE_TARGET_SIZE}"
    --precreate-storage-provider "${PRECREATE_STORAGE_PROVIDER}"
    --group-ping-timeout-ms "${GROUP_PING_TIMEOUT_MS}"
    --group-ping-interval-min-ms "${GROUP_PING_INTERVAL_MIN_MS}"
    --group-ping-interval-max-ms "${GROUP_PING_INTERVAL_MAX_MS}"
    --group-ping-max-timeouts "${GROUP_PING_MAX_TIMEOUTS}"
    --producer-wait-mode "${PRODUCER_WAIT_MODE}"
    --producer-flush-mode "${PRODUCER_FLUSH_MODE}"
    --protocol "${PROTOCOL}"
    --deployment-mode "${DEPLOYMENT_MODE}"
    --node-count "${NODE_COUNT}"
    --client-count "${CLIENT_COUNT}"
    --slurm-partition "${SLURM_PARTITION}"
    --slurm-time "${SLURM_TIME}"
  )
  if [[ -n "${SLURM_NODELIST}" ]]; then
    RECURSIVE_ARGS+=(--slurm-nodelist "${SLURM_NODELIST}")
  fi
  SUBMIT_DIR="${RESULT_DIR}/slurm"
  mkdir -p "${SUBMIT_DIR}"
  BATCH_SCRIPT="${SUBMIT_DIR}/mofka-recursive.sbatch.sh"
  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf 'export MOFKA_INSIDE_ALLOCATION=%q\n' 1
    printf 'export PHASE0_RESULT_DIR=%q\n' "${RESULT_DIR}"
    printf 'exec %q' "$0"
    printf ' %q' "${RECURSIVE_ARGS[@]}"
    printf '\n'
  } > "${BATCH_SCRIPT}"
  chmod +x "${BATCH_SCRIPT}"
  exec sbatch \
    --wait \
    --parsable \
    --job-name="mofka-${WORKFLOW}" \
    --partition="${SLURM_PARTITION}" \
    --nodes="${NODE_COUNT}" \
    --ntasks="${NODE_COUNT}" \
    --time="${SLURM_TIME}" \
    "${SBATCH_NODELIST_ARGS[@]}" \
    --output="${SUBMIT_DIR}/sbatch.stdout.log" \
    --error="${SUBMIT_DIR}/sbatch.stderr.log" \
    "${BATCH_SCRIPT}"
fi

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
  --storage-target-type "${STORAGE_TARGET_TYPE}" \
  --storage-target-size "${STORAGE_TARGET_SIZE}" \
  --precreate-storage-provider "${PRECREATE_STORAGE_PROVIDER}" \
  --group-ping-timeout-ms "${GROUP_PING_TIMEOUT_MS}" \
  --group-ping-interval-min-ms "${GROUP_PING_INTERVAL_MIN_MS}" \
  --group-ping-interval-max-ms "${GROUP_PING_INTERVAL_MAX_MS}" \
  --group-ping-max-timeouts "${GROUP_PING_MAX_TIMEOUTS}" \
  --wait-seconds "${LAUNCH_WAIT_SECONDS}"

mofka_export_spack_runtime_env

bedrock-query "${PROTOCOL}" -f "${MOFKA_RESULT_DIR}/mofka.json" -p \
  > "${MOFKA_RESULT_DIR}/bedrock-query-before-benchmark.json"

python3 "${SCRIPT_DIR}/mofka_append_benchmark.py" \
  --group-file "${MOFKA_RESULT_DIR}/mofka.json" \
  --result-dir "${RESULT_DIR}" \
  --operation-count "${OPERATION_COUNT}" \
  --message-size-bytes "${MESSAGE_SIZE_BYTES}" \
  --node-count "${NODE_COUNT}" \
  --client-count "${CLIENT_COUNT}" \
  --deployment-mode "${DEPLOYMENT_MODE}" \
  --workflow "${WORKFLOW}" \
  --partition-type "${PARTITION_TYPE}" \
  --storage-target-type "${STORAGE_TARGET_TYPE}" \
  --storage-target-size "${STORAGE_TARGET_SIZE}" \
  --precreate-storage-provider "${PRECREATE_STORAGE_PROVIDER}" \
  --group-ping-timeout-ms "${GROUP_PING_TIMEOUT_MS}" \
  --group-ping-interval-min-ms "${GROUP_PING_INTERVAL_MIN_MS}" \
  --group-ping-interval-max-ms "${GROUP_PING_INTERVAL_MAX_MS}" \
  --group-ping-max-timeouts "${GROUP_PING_MAX_TIMEOUTS}" \
  --producer-wait-mode "${PRODUCER_WAIT_MODE}" \
  --producer-flush-mode "${PRODUCER_FLUSH_MODE}" \
  --storage-path-root "${MOFKA_RESULT_DIR}/storage-targets" \
  > "${MOFKA_RESULT_DIR}/append-benchmark.stdout.log" \
  2> "${MOFKA_RESULT_DIR}/append-benchmark.stderr.log"

cat > "${RESULT_DIR}/summary.md" <<EOF
# Mofka Append Smoke

- system: Mofka
- workflow: ${WORKFLOW}
- deployment_mode: ${DEPLOYMENT_MODE}
- protocol: ${PROTOCOL}
- partition_type: ${PARTITION_TYPE}
- storage_target_type: ${STORAGE_TARGET_TYPE}
- storage_target_size: ${STORAGE_TARGET_SIZE}
- precreate_storage_provider: ${PRECREATE_STORAGE_PROVIDER}
- group_ping_timeout_ms: ${GROUP_PING_TIMEOUT_MS}
- group_ping_interval_min_ms: ${GROUP_PING_INTERVAL_MIN_MS}
- group_ping_interval_max_ms: ${GROUP_PING_INTERVAL_MAX_MS}
- group_ping_max_timeouts: ${GROUP_PING_MAX_TIMEOUTS}
- producer_wait_mode: ${PRODUCER_WAIT_MODE}
- producer_flush_mode: ${PRODUCER_FLUSH_MODE}
- node_count: ${NODE_COUNT}
- nodes: ${NODE_COUNT}
- client_count: ${CLIENT_COUNT}
- parallel_clients: ${CLIENT_COUNT}
- parallel_client_count: ${CLIENT_COUNT}
- operation_count: ${OPERATION_COUNT}
- operation_count_per_client: ${OPERATION_COUNT}
- message_count_per_client: ${OPERATION_COUNT}
- messages_per_client: ${OPERATION_COUNT}
- total_operation_count: $((OPERATION_COUNT * CLIENT_COUNT))
- total_message_count: $((OPERATION_COUNT * CLIENT_COUNT))
- total_messages: $((OPERATION_COUNT * CLIENT_COUNT))
- message_size_bytes: ${MESSAGE_SIZE_BYTES}
- total_payload_bytes: $((OPERATION_COUNT * CLIENT_COUNT * MESSAGE_SIZE_BYTES))
- metrics: mofka/metrics.json

This validation confirms the Mofka append path and metrics plumbing. A bare_metal run contributes distributed evidence; a local_validation run does not satisfy the final distributed benchmark requirement.
EOF
