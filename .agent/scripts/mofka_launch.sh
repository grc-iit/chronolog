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
      echo "${label} must not be tmpfs-backed (${resolved}); Mofka PMDK/abtio storage evidence requires persistent/shared storage or explicit local NVMe/SSD" >&2
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

mofka_wait_for_group_member_count() {
  local group_file="$1"
  local expected_count="$2"
  local timeout_seconds="$3"
  local deadline=$((SECONDS + timeout_seconds))
  local observed_count

  while (( SECONDS < deadline )); do
    observed_count="$(
      python3 - "${group_file}" <<'PY' 2>/dev/null || true
import json
import sys
from pathlib import Path

try:
    data = json.loads(Path(sys.argv[1]).read_text())
    print(len(data.get("members", [])))
except Exception:
    print(0)
PY
    )"
    if [[ "${observed_count}" -ge "${expected_count}" ]]; then
      return 0
    fi
    sleep 1
  done

  echo "Mofka group file ${group_file} has ${observed_count:-0} members; expected ${expected_count}" >&2
  return 1
}

usage() {
  cat <<'USAGE'
Usage: mofka_launch.sh [options]

Launch a Phase 0 Mofka fixed baseline using Bedrock. Bare-metal distributed
deployment is the target; local single-node mode is only a validation.

Options:
  --result-dir DIR          Existing or new result directory to use.
  --node-count N            Total server processes/nodes requested. Default: 1.
  --protocol PROTOCOL       Mercury protocol. Default: ofi+tcp for bare metal,
                            na+sm for local_validation.
  --deployment-mode MODE    bare_metal or local_validation. Default: bare_metal.
  --slurm-partition NAME    Partition for bare_metal multi-node launch. Default: debug.
  --slurm-nodelist LIST     Comma-separated or SLURM-style host list to pin nodes.
  --slurm-time TIME         Time limit for bare_metal srun commands. Default: 00:30:00.
  --storage-target-type TYPE Warabi target type: pmdk, abtio, or memory. Default: pmdk.
  --storage-path-root DIR   Root directory for non-memory Warabi targets. Default: result dir.
  --storage-target-size N   Size in bytes for pmdk targets. Default: 67108864.
  --precreate-storage-provider yes|no
                            Pre-create a Warabi provider in the Bedrock storage
                            config. Default: yes.
  --group-ping-timeout-ms N Centralized Flock group ping timeout. Default: 1000.
  --group-ping-interval-min-ms N
                            Centralized Flock group minimum ping interval. Default: 1000.
  --group-ping-interval-max-ms N
                            Centralized Flock group maximum ping interval. Default: 1000.
  --group-ping-max-timeouts N
                            Centralized Flock group missed-ping eviction threshold. Default: 3.
  --wait-seconds N          Wait for group file creation. Default: 30.
  -h, --help                Show this help.
USAGE
}

RESULT_DIR="${PHASE0_RESULT_DIR:-}"
NODE_COUNT="${MOFKA_NODE_COUNT:-1}"
DEPLOYMENT_MODE="${MOFKA_DEPLOYMENT_MODE:-bare_metal}"
PROTOCOL="${MOFKA_PROTOCOL:-}"
WAIT_SECONDS="${MOFKA_WAIT_SECONDS:-30}"
SLURM_PARTITION="${MOFKA_SLURM_PARTITION:-debug}"
SLURM_NODELIST="${MOFKA_SLURM_NODELIST:-}"
SLURM_TIME="${MOFKA_SLURM_TIME:-00:30:00}"
STORAGE_TARGET_TYPE="${MOFKA_STORAGE_TARGET_TYPE:-pmdk}"
STORAGE_PATH_ROOT="${MOFKA_STORAGE_PATH_ROOT:-}"
STORAGE_TARGET_SIZE="${MOFKA_STORAGE_TARGET_SIZE:-67108864}"
PRECREATE_STORAGE_PROVIDER="${MOFKA_PRECREATE_STORAGE_PROVIDER:-yes}"
GROUP_PING_TIMEOUT_MS="${MOFKA_GROUP_PING_TIMEOUT_MS:-1000}"
GROUP_PING_INTERVAL_MIN_MS="${MOFKA_GROUP_PING_INTERVAL_MIN_MS:-1000}"
GROUP_PING_INTERVAL_MAX_MS="${MOFKA_GROUP_PING_INTERVAL_MAX_MS:-1000}"
GROUP_PING_MAX_TIMEOUTS="${MOFKA_GROUP_PING_MAX_TIMEOUTS:-3}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --result-dir)
      RESULT_DIR="$2"
      shift 2
      ;;
    --node-count)
      NODE_COUNT="$2"
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
    --storage-target-type)
      STORAGE_TARGET_TYPE="$2"
      shift 2
      ;;
    --storage-path-root)
      STORAGE_PATH_ROOT="$2"
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
    --wait-seconds)
      WAIT_SECONDS="$2"
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

if [[ "${DEPLOYMENT_MODE}" != "bare_metal" && "${DEPLOYMENT_MODE}" != "local_validation" ]]; then
  echo "Unsupported deployment mode: ${DEPLOYMENT_MODE}" >&2
  exit 2
fi

if [[ "${STORAGE_TARGET_TYPE}" != "pmdk" && "${STORAGE_TARGET_TYPE}" != "abtio" && "${STORAGE_TARGET_TYPE}" != "memory" ]]; then
  echo "Unsupported storage target type: ${STORAGE_TARGET_TYPE}" >&2
  exit 2
fi

if [[ "${PRECREATE_STORAGE_PROVIDER}" != "yes" && "${PRECREATE_STORAGE_PROVIDER}" != "no" ]]; then
  echo "Unsupported precreate storage provider mode: ${PRECREATE_STORAGE_PROVIDER}" >&2
  exit 2
fi

for numeric_value in \
  "group-ping-timeout-ms:${GROUP_PING_TIMEOUT_MS}" \
  "group-ping-interval-min-ms:${GROUP_PING_INTERVAL_MIN_MS}" \
  "group-ping-interval-max-ms:${GROUP_PING_INTERVAL_MAX_MS}" \
  "group-ping-max-timeouts:${GROUP_PING_MAX_TIMEOUTS}"; do
  numeric_name="${numeric_value%%:*}"
  numeric_payload="${numeric_value#*:}"
  if ! [[ "${numeric_payload}" =~ ^[0-9]+$ ]] || [[ "${numeric_payload}" -le 0 ]]; then
    echo "--${numeric_name} must be a positive integer: ${numeric_payload}" >&2
    exit 2
  fi
done
if [[ "${GROUP_PING_INTERVAL_MIN_MS}" -gt "${GROUP_PING_INTERVAL_MAX_MS}" ]]; then
  echo "--group-ping-interval-min-ms must be <= --group-ping-interval-max-ms" >&2
  exit 2
fi
if [[ "${GROUP_PING_MAX_TIMEOUTS}" -le 1 ]]; then
  echo "--group-ping-max-timeouts must be > 1" >&2
  exit 2
fi

GROUP_PING_TIMEOUT_JSON="${GROUP_PING_TIMEOUT_MS}.0"
GROUP_PING_INTERVAL_MIN_JSON="${GROUP_PING_INTERVAL_MIN_MS}.0"
GROUP_PING_INTERVAL_MAX_JSON="${GROUP_PING_INTERVAL_MAX_MS}.0"

if [[ -z "${PROTOCOL}" ]]; then
  if [[ "${DEPLOYMENT_MODE}" == "local_validation" ]]; then
    PROTOCOL="na+sm"
  else
    PROTOCOL="ofi+tcp"
  fi
fi

RESULT_DIR="$(phase0_make_result_dir "${RESULT_DIR}")"
reject_tmpfs_storage_path "--result-dir" "${RESULT_DIR}"
CONFIG_DIR="${RESULT_DIR}/config"
MOFKA_RESULT_DIR="${RESULT_DIR}/mofka"
MOFKA_LOG_DIR="${MOFKA_RESULT_DIR}/logs"
MOFKA_PID_DIR="${MOFKA_RESULT_DIR}/pids"
GROUP_FILE="${MOFKA_RESULT_DIR}/mofka.json"
if [[ -z "${STORAGE_PATH_ROOT}" ]]; then
  STORAGE_PATH_ROOT="${MOFKA_RESULT_DIR}/storage-targets"
fi
if [[ "${STORAGE_TARGET_TYPE}" != "memory" ]]; then
  reject_tmpfs_storage_path "--storage-path-root" "${STORAGE_PATH_ROOT}"
fi
STORAGE_COUNT=$(( NODE_COUNT > 1 ? NODE_COUNT - 1 : 1 ))
SERVER_PROCESS_COUNT=$(( STORAGE_COUNT + 1 ))
declare -a SELECTED_SLURM_NODES=()

mkdir -p "${CONFIG_DIR}" "${MOFKA_LOG_DIR}" "${MOFKA_PID_DIR}" "${STORAGE_PATH_ROOT}"

exec > >(tee -a "${MOFKA_RESULT_DIR}/stdout.log")
exec 2> >(tee -a "${MOFKA_RESULT_DIR}/stderr.log" >&2)

mofka_export_spack_runtime_env

mofka_command_summary > "${CONFIG_DIR}/mofka-command-paths.txt"
mofka_require_commands

if [[ "${DEPLOYMENT_MODE}" == "bare_metal" && "${NODE_COUNT}" -gt 1 ]]; then
  if ! command -v srun >/dev/null 2>&1; then
    echo "srun is required for bare_metal distributed Mofka launch" >&2
    exit 1
  fi

  if [[ -n "${SLURM_NODELIST}" ]]; then
    if command -v scontrol >/dev/null 2>&1; then
      mapfile -t SELECTED_SLURM_NODES < <(scontrol show hostnames "${SLURM_NODELIST}")
    else
      IFS=',' read -r -a SELECTED_SLURM_NODES <<< "${SLURM_NODELIST}"
    fi
  elif [[ -n "${SLURM_JOB_NODELIST:-}" ]]; then
    if command -v scontrol >/dev/null 2>&1; then
      mapfile -t SELECTED_SLURM_NODES < <(scontrol show hostnames "${SLURM_JOB_NODELIST}")
    else
      IFS=',' read -r -a SELECTED_SLURM_NODES <<< "${SLURM_JOB_NODELIST}"
    fi
  else
    mapfile -t SELECTED_SLURM_NODES < <(
      sinfo -N -h -p "${SLURM_PARTITION}" -t idle -o '%N' | sort -u | head -n "${NODE_COUNT}"
    )
  fi

  if [[ "${#SELECTED_SLURM_NODES[@]}" -lt "${NODE_COUNT}" ]]; then
    echo "Only found ${#SELECTED_SLURM_NODES[@]} usable SLURM nodes for requested node count ${NODE_COUNT}" >&2
    exit 1
  fi

  printf '%s\n' "${SELECTED_SLURM_NODES[@]:0:${NODE_COUNT}}" > "${CONFIG_DIR}/mofka-slurm-nodes.txt"
fi

MASTER_CONFIG="${CONFIG_DIR}/mofka-master.json"
LAUNCH_ENV="${CONFIG_DIR}/mofka-launch.env"

cat > "${MASTER_CONFIG}" <<EOF
{
  "libraries": [
    "libflock-bedrock-module.so",
    "libyokan-bedrock-module.so"
  ],
  "providers": [
    {
      "name": "phase0_group_manager",
      "type": "flock",
      "provider_id": 1,
      "config": {
        "bootstrap": "self",
        "file": "${GROUP_FILE}",
        "group": {
          "type": "centralized",
          "config": {
            "ping_timeout_ms": ${GROUP_PING_TIMEOUT_JSON},
            "ping_interval_ms": [${GROUP_PING_INTERVAL_MIN_JSON}, ${GROUP_PING_INTERVAL_MAX_JSON}],
            "ping_max_num_timeouts": ${GROUP_PING_MAX_TIMEOUTS}
          }
        }
      }
    },
    {
      "name": "phase0_master",
      "provider_id": 2,
      "type": "yokan",
      "tags": [ "mofka:master" ],
      "config": {
        "database": { "type": "map" }
      }
    }
  ]
}
EOF

write_storage_config() {
  local storage_index="$1"
  local storage_config="$2"
  local storage_path="${STORAGE_PATH_ROOT}/storage-${storage_index}"
  local target_config
  local provider_entries

  if [[ "${STORAGE_TARGET_TYPE}" == "memory" ]]; then
    target_config='{ "type": "memory" }'
  elif [[ "${STORAGE_TARGET_TYPE}" == "pmdk" ]]; then
    mkdir -p "$(dirname "${storage_path}")"
    target_config=$(printf '{ "type": "pmdk", "config": { "path": "%s", "create_if_missing_with_size": %s, "override_if_exists": true } }' "${storage_path}.pool" "${STORAGE_TARGET_SIZE}")
  else
    mkdir -p "$(dirname "${storage_path}")"
    target_config=$(printf '{ "type": "abtio", "config": { "path": "%s", "create_if_missing": true, "override_if_exists": true } }' "${storage_path}.abtio")
  fi

  if [[ "${PRECREATE_STORAGE_PROVIDER}" == "yes" ]]; then
    provider_entries=$(cat <<EOF
,
    {
      "name": "phase0_data_provider",
      "provider_id": 4,
      "type": "warabi",
      "tags": [ "mofka:data" ],
      "config": {
        "target": ${target_config}
      }
    }
EOF
)
  else
    provider_entries=""
  fi

  cat > "${storage_config}" <<EOF
{
  "libraries": [
    "libflock-bedrock-module.so",
    "libyokan-bedrock-module.so",
    "libwarabi-bedrock-module.so",
    "libmofka-bedrock-module.so"
  ],
  "providers": [
    {
      "name": "phase0_group_manager",
      "type": "flock",
      "provider_id": 1,
      "config": {
        "bootstrap": "join",
        "file": "${GROUP_FILE}",
        "group": {
          "type": "centralized",
          "config": {
            "ping_timeout_ms": ${GROUP_PING_TIMEOUT_JSON},
            "ping_interval_ms": [${GROUP_PING_INTERVAL_MIN_JSON}, ${GROUP_PING_INTERVAL_MAX_JSON}],
            "ping_max_num_timeouts": ${GROUP_PING_MAX_TIMEOUTS}
          }
        }
      }
    },
    {
      "name": "phase0_metadata_provider",
      "provider_id": 3,
      "type": "yokan",
      "tags": [ "mofka:metadata" ],
      "config": {
        "database": { "type": "map" }
      }
    }${provider_entries}
  ]
}
EOF
}

declare -a STORAGE_CONFIGS=()
for storage_index in $(seq 1 "${STORAGE_COUNT}"); do
  storage_config="${CONFIG_DIR}/mofka-storage-${storage_index}.json"
  write_storage_config "${storage_index}" "${storage_config}"
  STORAGE_CONFIGS+=("${storage_config}")
done

cat > "${LAUNCH_ENV}" <<EOF
MOFKA_RESULT_DIR='${MOFKA_RESULT_DIR}'
MOFKA_GROUP_FILE='${GROUP_FILE}'
MOFKA_MASTER_CONFIG='${MASTER_CONFIG}'
MOFKA_STORAGE_CONFIG_DIR='${CONFIG_DIR}'
MOFKA_PROTOCOL='${PROTOCOL}'
MOFKA_DEPLOYMENT_MODE='${DEPLOYMENT_MODE}'
MOFKA_NODE_COUNT='${NODE_COUNT}'
MOFKA_STORAGE_TARGET_TYPE='${STORAGE_TARGET_TYPE}'
MOFKA_STORAGE_PATH_ROOT='${STORAGE_PATH_ROOT}'
MOFKA_STORAGE_TARGET_SIZE='${STORAGE_TARGET_SIZE}'
MOFKA_PRECREATE_STORAGE_PROVIDER='${PRECREATE_STORAGE_PROVIDER}'
MOFKA_GROUP_PING_TIMEOUT_MS='${GROUP_PING_TIMEOUT_MS}'
MOFKA_GROUP_PING_INTERVAL_MIN_MS='${GROUP_PING_INTERVAL_MIN_MS}'
MOFKA_GROUP_PING_INTERVAL_MAX_MS='${GROUP_PING_INTERVAL_MAX_MS}'
MOFKA_GROUP_PING_MAX_TIMEOUTS='${GROUP_PING_MAX_TIMEOUTS}'
MOFKA_SPACK_SPEC='${MOFKA_SPACK_SPEC:-mofka@0.6.4+python~mpi~benchmark~kafka}'
MOFKA_SLURM_PARTITION='${SLURM_PARTITION}'
MOFKA_SLURM_NODELIST='${SLURM_NODELIST}'
MOFKA_SLURM_TIME='${SLURM_TIME}'
EOF

{
  echo "deployment_mode=${DEPLOYMENT_MODE}"
  echo "node_count=${NODE_COUNT}"
  echo "server_process_count=${SERVER_PROCESS_COUNT}"
  echo "storage_process_count=${STORAGE_COUNT}"
  echo "protocol=${PROTOCOL}"
  echo "group_file=${GROUP_FILE}"
  echo "master_config=${MASTER_CONFIG}"
  echo "storage_config_dir=${CONFIG_DIR}"
  echo "storage_target_type=${STORAGE_TARGET_TYPE}"
  echo "storage_path_root=${STORAGE_PATH_ROOT}"
  echo "storage_target_size=${STORAGE_TARGET_SIZE}"
  echo "precreate_storage_provider=${PRECREATE_STORAGE_PROVIDER}"
  echo "group_ping_timeout_ms=${GROUP_PING_TIMEOUT_MS}"
  echo "group_ping_interval_min_ms=${GROUP_PING_INTERVAL_MIN_MS}"
  echo "group_ping_interval_max_ms=${GROUP_PING_INTERVAL_MAX_MS}"
  echo "group_ping_max_timeouts=${GROUP_PING_MAX_TIMEOUTS}"
  echo "mofka_spack_spec=${MOFKA_SPACK_SPEC:-mofka@0.6.4+python~mpi~benchmark~kafka}"
  echo "slurm_partition=${SLURM_PARTITION}"
  echo "slurm_nodelist=${SLURM_NODELIST}"
  echo "slurm_time=${SLURM_TIME}"
  echo "slurm_nodes_file=${CONFIG_DIR}/mofka-slurm-nodes.txt"
  echo "bedrock=$(command -v bedrock)"
  echo "mofkactl=$(command -v mofkactl)"
  echo "hostname=$(hostname)"
  echo "slurm_job_id=${SLURM_JOB_ID:-}"
  echo "slurm_job_nodelist=${SLURM_JOB_NODELIST:-}"
  echo "ld_library_path_file=${CONFIG_DIR}/mofka-ld-library-path.txt"
  echo "pythonpath_file=${CONFIG_DIR}/mofka-pythonpath.txt"
} > "${CONFIG_DIR}/mofka-config-manifest.env"

printf '%s\n' "${LD_LIBRARY_PATH:-}" | tr ':' '\n' > "${CONFIG_DIR}/mofka-ld-library-path.txt"
printf '%s\n' "${PYTHONPATH:-}" | tr ':' '\n' > "${CONFIG_DIR}/mofka-pythonpath.txt"

echo "Mofka result directory: ${RESULT_DIR}"
echo "Deployment mode: ${DEPLOYMENT_MODE}"
echo "Node count: ${NODE_COUNT}"
echo "Protocol: ${PROTOCOL}"
echo "Storage target type: ${STORAGE_TARGET_TYPE}"
echo "Storage path root: ${STORAGE_PATH_ROOT}"

if [[ "${DEPLOYMENT_MODE}" == "bare_metal" && "${NODE_COUNT}" -gt 1 ]]; then
  MASTER_NODE="${SELECTED_SLURM_NODES[0]}"
  echo "Master node: ${MASTER_NODE}"
  srun --partition="${SLURM_PARTITION}" --nodes=1 --ntasks=1 --exclusive \
    --time="${SLURM_TIME}" --nodelist="${MASTER_NODE}" \
    bash -lc "hostname; exec bedrock '${PROTOCOL}' -c '${MASTER_CONFIG}'" \
    > "${MOFKA_LOG_DIR}/mofka-master.stdout.log" \
    2> "${MOFKA_LOG_DIR}/mofka-master.stderr.log" &
else
  nohup bedrock "${PROTOCOL}" -c "${MASTER_CONFIG}" \
    > "${MOFKA_LOG_DIR}/mofka-master.stdout.log" \
    2> "${MOFKA_LOG_DIR}/mofka-master.stderr.log" &
fi

MASTER_PID=$!
echo "${MASTER_PID}" > "${MOFKA_PID_DIR}/mofka-master.pid"

if ! mofka_wait_for_file "${GROUP_FILE}" "${WAIT_SECONDS}"; then
  echo "Mofka group file was not created at ${GROUP_FILE}" >&2
  exit 1
fi

for storage_index in $(seq 1 "${STORAGE_COUNT}"); do
  STORAGE_CONFIG="${STORAGE_CONFIGS[$((storage_index - 1))]}"
  if [[ "${DEPLOYMENT_MODE}" == "bare_metal" && "${NODE_COUNT}" -gt 1 ]]; then
    STORAGE_NODE="${SELECTED_SLURM_NODES[${storage_index}]}"
    echo "Storage ${storage_index} node: ${STORAGE_NODE}"
    srun --partition="${SLURM_PARTITION}" --nodes=1 --ntasks=1 --exclusive \
      --time="${SLURM_TIME}" --nodelist="${STORAGE_NODE}" \
      bash -lc "hostname; exec bedrock '${PROTOCOL}' -c '${STORAGE_CONFIG}'" \
      > "${MOFKA_LOG_DIR}/mofka-storage-${storage_index}.stdout.log" \
      2> "${MOFKA_LOG_DIR}/mofka-storage-${storage_index}.stderr.log" &
  else
    nohup bedrock "${PROTOCOL}" -c "${STORAGE_CONFIG}" \
      > "${MOFKA_LOG_DIR}/mofka-storage-${storage_index}.stdout.log" \
      2> "${MOFKA_LOG_DIR}/mofka-storage-${storage_index}.stderr.log" &
  fi
  echo "$!" > "${MOFKA_PID_DIR}/mofka-storage-${storage_index}.pid"
  if ! mofka_wait_for_group_member_count "${GROUP_FILE}" "$((storage_index + 1))" "${WAIT_SECONDS}"; then
    exit 1
  fi
done

if ! mofka_wait_for_group_member_count "${GROUP_FILE}" "${SERVER_PROCESS_COUNT}" "${WAIT_SECONDS}"; then
  exit 1
fi

sleep 2
for pid_file in "${MOFKA_PID_DIR}"/*.pid; do
  process_name="$(basename "${pid_file}" .pid)"
  process_id="$(cat "${pid_file}")"
  if ! kill -0 "${process_id}" >/dev/null 2>&1; then
    echo "${process_name} exited during startup; inspect ${MOFKA_LOG_DIR}/${process_name}.stderr.log" >&2
    exit 1
  fi
done

echo "Mofka fixed baseline is running"
