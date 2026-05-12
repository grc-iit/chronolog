#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.agent/scripts/mofka_common.sh
source "${SCRIPT_DIR}/mofka_common.sh"

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

if [[ -z "${PROTOCOL}" ]]; then
  if [[ "${DEPLOYMENT_MODE}" == "local_validation" ]]; then
    PROTOCOL="na+sm"
  else
    PROTOCOL="ofi+tcp"
  fi
fi

RESULT_DIR="$(phase0_make_result_dir "${RESULT_DIR}")"
CONFIG_DIR="${RESULT_DIR}/config"
MOFKA_RESULT_DIR="${RESULT_DIR}/mofka"
MOFKA_LOG_DIR="${MOFKA_RESULT_DIR}/logs"
MOFKA_PID_DIR="${MOFKA_RESULT_DIR}/pids"
GROUP_FILE="${MOFKA_RESULT_DIR}/mofka.json"
STORAGE_COUNT=$(( NODE_COUNT > 1 ? NODE_COUNT - 1 : 1 ))
SERVER_PROCESS_COUNT=$(( STORAGE_COUNT + 1 ))
declare -a SELECTED_SLURM_NODES=()

mkdir -p "${CONFIG_DIR}" "${MOFKA_LOG_DIR}" "${MOFKA_PID_DIR}"

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
STORAGE_CONFIG="${CONFIG_DIR}/mofka-storage.json"
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
        "group": { "type": "centralized" }
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

cat > "${STORAGE_CONFIG}" <<EOF
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
        "group": { "type": "centralized" }
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
    },
    {
      "name": "phase0_data_provider",
      "provider_id": 4,
      "type": "warabi",
      "tags": [ "mofka:data" ],
      "config": {
        "target": { "type": "memory" }
      }
    }
  ]
}
EOF

cat > "${LAUNCH_ENV}" <<EOF
MOFKA_RESULT_DIR='${MOFKA_RESULT_DIR}'
MOFKA_GROUP_FILE='${GROUP_FILE}'
MOFKA_MASTER_CONFIG='${MASTER_CONFIG}'
MOFKA_STORAGE_CONFIG='${STORAGE_CONFIG}'
MOFKA_PROTOCOL='${PROTOCOL}'
MOFKA_DEPLOYMENT_MODE='${DEPLOYMENT_MODE}'
MOFKA_NODE_COUNT='${NODE_COUNT}'
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
  echo "storage_config=${STORAGE_CONFIG}"
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
done

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
