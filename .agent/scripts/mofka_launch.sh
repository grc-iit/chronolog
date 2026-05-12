#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.agent/scripts/mofka_common.sh
source "${SCRIPT_DIR}/mofka_common.sh"

usage() {
  cat <<'USAGE'
Usage: mofka_launch.sh [options]

Launch a Phase 0 Mofka fixed baseline using Bedrock. Bare-metal distributed
deployment is the target; local single-node mode is only a smoke validation.

Options:
  --result-dir DIR          Existing or new result directory to use.
  --node-count N            Total server processes/nodes requested. Default: 1.
  --protocol PROTOCOL       Mercury protocol. Default: ofi+tcp for bare metal,
                            na+sm for local_smoke.
  --deployment-mode MODE    bare_metal or local_smoke. Default: bare_metal.
  --wait-seconds N          Wait for group file creation. Default: 30.
  -h, --help                Show this help.
USAGE
}

RESULT_DIR="${PHASE0_RESULT_DIR:-}"
NODE_COUNT="${MOFKA_NODE_COUNT:-1}"
DEPLOYMENT_MODE="${MOFKA_DEPLOYMENT_MODE:-bare_metal}"
PROTOCOL="${MOFKA_PROTOCOL:-}"
WAIT_SECONDS="${MOFKA_WAIT_SECONDS:-30}"

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

if [[ "${DEPLOYMENT_MODE}" != "bare_metal" && "${DEPLOYMENT_MODE}" != "local_smoke" ]]; then
  echo "Unsupported deployment mode: ${DEPLOYMENT_MODE}" >&2
  exit 2
fi

if [[ -z "${PROTOCOL}" ]]; then
  if [[ "${DEPLOYMENT_MODE}" == "local_smoke" ]]; then
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

mkdir -p "${CONFIG_DIR}" "${MOFKA_LOG_DIR}" "${MOFKA_PID_DIR}"

exec > >(tee -a "${MOFKA_RESULT_DIR}/stdout.log")
exec 2> >(tee -a "${MOFKA_RESULT_DIR}/stderr.log" >&2)

mofka_command_summary > "${CONFIG_DIR}/mofka-command-paths.txt"
mofka_require_commands

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
      "tags": [ "mofka:controller" ],
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
EOF

echo "Mofka result directory: ${RESULT_DIR}"
echo "Deployment mode: ${DEPLOYMENT_MODE}"
echo "Node count: ${NODE_COUNT}"
echo "Protocol: ${PROTOCOL}"

if [[ "${DEPLOYMENT_MODE}" == "bare_metal" && "${NODE_COUNT}" -gt 1 ]]; then
  if ! command -v srun >/dev/null 2>&1; then
    echo "srun is required for bare_metal distributed Mofka launch" >&2
    exit 1
  fi
  srun --nodes=1 --ntasks=1 bedrock "${PROTOCOL}" -c "${MASTER_CONFIG}" \
    > "${MOFKA_LOG_DIR}/mofka-master.stdout.log" \
    2> "${MOFKA_LOG_DIR}/mofka-master.stderr.log" &
else
  bedrock "${PROTOCOL}" -c "${MASTER_CONFIG}" \
    > "${MOFKA_LOG_DIR}/mofka-master.stdout.log" \
    2> "${MOFKA_LOG_DIR}/mofka-master.stderr.log" &
fi

MASTER_PID=$!
echo "${MASTER_PID}" > "${MOFKA_PID_DIR}/mofka-master.pid"

if ! mofka_wait_for_file "${GROUP_FILE}" "${WAIT_SECONDS}"; then
  echo "Mofka group file was not created at ${GROUP_FILE}" >&2
  exit 1
fi

STORAGE_COUNT=$(( NODE_COUNT > 1 ? NODE_COUNT - 1 : 1 ))
for storage_index in $(seq 1 "${STORAGE_COUNT}"); do
  if [[ "${DEPLOYMENT_MODE}" == "bare_metal" && "${NODE_COUNT}" -gt 1 ]]; then
    srun --nodes=1 --ntasks=1 bedrock "${PROTOCOL}" -c "${STORAGE_CONFIG}" \
      > "${MOFKA_LOG_DIR}/mofka-storage-${storage_index}.stdout.log" \
      2> "${MOFKA_LOG_DIR}/mofka-storage-${storage_index}.stderr.log" &
  else
    bedrock "${PROTOCOL}" -c "${STORAGE_CONFIG}" \
      > "${MOFKA_LOG_DIR}/mofka-storage-${storage_index}.stdout.log" \
      2> "${MOFKA_LOG_DIR}/mofka-storage-${storage_index}.stderr.log" &
  fi
  echo "$!" > "${MOFKA_PID_DIR}/mofka-storage-${storage_index}.pid"
done

echo "Mofka fixed baseline is running"
