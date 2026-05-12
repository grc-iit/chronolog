#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

usage() {
  cat <<'USAGE'
Usage: phase0_slurm_run.sh [options] -- command [args...]

Run a command under SLURM and write Phase 0 allocation evidence.

Options:
  --result-dir DIR       Existing or new result directory.
  --partition NAME       SLURM partition. Default: debug.
  --nodes N              Node count. Default: 2.
  --ntasks N             Task count. Default: same as nodes.
  --time HH:MM:SS        SLURM time limit. Default: 00:10:00.
  --job-name NAME        SLURM job name. Default: phase0-dist.
  -h, --help             Show this help.
USAGE
}

timestamp() {
  date +%Y%m%d-%H%M%S
}

RESULT_DIR="${PHASE0_RESULT_DIR:-}"
PARTITION="${PHASE0_SLURM_PARTITION:-debug}"
NODES="${PHASE0_SLURM_NODES:-2}"
NTASKS="${PHASE0_SLURM_NTASKS:-}"
TIME_LIMIT="${PHASE0_SLURM_TIME:-00:10:00}"
JOB_NAME="${PHASE0_SLURM_JOB_NAME:-phase0-dist}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --result-dir)
      RESULT_DIR="$2"
      shift 2
      ;;
    --partition)
      PARTITION="$2"
      shift 2
      ;;
    --nodes)
      NODES="$2"
      shift 2
      ;;
    --ntasks)
      NTASKS="$2"
      shift 2
      ;;
    --time)
      TIME_LIMIT="$2"
      shift 2
      ;;
    --job-name)
      JOB_NAME="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ $# -eq 0 ]]; then
  echo "No command provided" >&2
  usage >&2
  exit 2
fi

if [[ -z "${NTASKS}" ]]; then
  NTASKS="${NODES}"
fi

if [[ -z "${RESULT_DIR}" ]]; then
  RESULT_DIR="${REPO_ROOT}/.agent/results/$(timestamp)"
fi

mkdir -p "${RESULT_DIR}/config" "${RESULT_DIR}/slurm" "${RESULT_DIR}/logs"
RESULT_DIR="$(cd "${RESULT_DIR}" && pwd)"

COMMAND_FILE="${RESULT_DIR}/config/slurm-command.txt"
ENV_FILE="${RESULT_DIR}/config/slurm-run.env"
HOSTS_FILE="${RESULT_DIR}/config/slurm-hosts.txt"
NODE_CONFIG_FILE="${RESULT_DIR}/config/slurm-node-config.txt"

printf '%q ' "$@" > "${COMMAND_FILE}"
printf '\n' >> "${COMMAND_FILE}"

cat > "${ENV_FILE}" <<EOF
partition=${PARTITION}
nodes=${NODES}
ntasks=${NTASKS}
time_limit=${TIME_LIMIT}
job_name=${JOB_NAME}
result_dir=${RESULT_DIR}
repo_root=${REPO_ROOT}
EOF

echo "Phase 0 SLURM result directory: ${RESULT_DIR}"
echo "Command: $(cat "${COMMAND_FILE}")"

srun --partition="${PARTITION}" \
  --nodes="${NODES}" \
  --ntasks="${NTASKS}" \
  --time="${TIME_LIMIT}" \
  --job-name="${JOB_NAME}" \
  hostname > "${HOSTS_FILE}" \
  2> "${RESULT_DIR}/slurm/hostname.stderr.log"

srun --partition="${PARTITION}" \
  --nodes="${NODES}" \
  --ntasks="${NTASKS}" \
  --time="${TIME_LIMIT}" \
  --job-name="${JOB_NAME}-cfg" \
  "${SCRIPT_DIR}/phase0_collect_node_config.sh" \
  > "${NODE_CONFIG_FILE}" \
  2> "${RESULT_DIR}/slurm/node-config.stderr.log"

set +e
srun --partition="${PARTITION}" \
  --nodes="${NODES}" \
  --ntasks="${NTASKS}" \
  --time="${TIME_LIMIT}" \
  --job-name="${JOB_NAME}" \
  "$@" > "${RESULT_DIR}/slurm/stdout.log" \
  2> "${RESULT_DIR}/slurm/stderr.log"
STATUS=$?
set -e

cat > "${RESULT_DIR}/slurm/status.env" <<EOF
status=${STATUS}
EOF

exit "${STATUS}"
