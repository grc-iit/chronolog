#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.agent/scripts/mofka_common.sh
source "${SCRIPT_DIR}/mofka_common.sh"

usage() {
  cat <<'USAGE'
Usage: mofka_stop.sh [options]

Stop a Phase 0 Mofka fixed baseline launched by mofka_launch.sh.

Options:
  --result-dir DIR        Result directory containing mofka/pids.
  --timeout-seconds N     Graceful shutdown timeout per process. Default: 20.
  -h, --help              Show this help.
USAGE
}

find_latest_mofka_result_dir() {
  local latest
  latest="$(find "${PHASE0_REPO_ROOT}/.agent/results" -mindepth 1 -maxdepth 1 -type d \
    -exec test -d "{}/mofka/pids" \; -print 2>/dev/null | sort | tail -1)"
  if [[ -n "${latest}" ]]; then
    echo "${latest}"
    return 0
  fi
  return 1
}

stop_pid_file() {
  local label="$1"
  local pid_file="$2"
  local timeout_seconds="$3"

  if [[ ! -f "${pid_file}" ]]; then
    return 0
  fi

  local pid
  pid="$(tr -d '[:space:]' < "${pid_file}")"
  if [[ -z "${pid}" ]]; then
    return 0
  fi

  if ! kill -0 "${pid}" >/dev/null 2>&1; then
    echo "${label} pid ${pid} is not running"
    return 0
  fi

  echo "Stopping ${label} pid ${pid}"
  kill "${pid}" >/dev/null 2>&1 || true

  local deadline=$((SECONDS + timeout_seconds))
  while (( SECONDS < deadline )); do
    if ! kill -0 "${pid}" >/dev/null 2>&1; then
      echo "${label} pid ${pid} stopped"
      return 0
    fi
    sleep 1
  done

  echo "${label} pid ${pid} did not stop within ${timeout_seconds}s; sending SIGKILL"
  kill -KILL "${pid}" >/dev/null 2>&1 || true
}

RESULT_DIR="${PHASE0_RESULT_DIR:-}"
TIMEOUT_SECONDS="${MOFKA_STOP_TIMEOUT_SECONDS:-20}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --result-dir)
      RESULT_DIR="$2"
      shift 2
      ;;
    --timeout-seconds)
      TIMEOUT_SECONDS="$2"
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

if [[ -z "${RESULT_DIR}" ]]; then
  RESULT_DIR="$(find_latest_mofka_result_dir)" || {
    echo "No Mofka result directory found; pass --result-dir" >&2
    exit 1
  }
fi

RESULT_DIR="$(cd "${RESULT_DIR}" && pwd)"
MOFKA_RESULT_DIR="${RESULT_DIR}/mofka"

mkdir -p "${MOFKA_RESULT_DIR}"
exec > >(tee -a "${MOFKA_RESULT_DIR}/stdout.log")
exec 2> >(tee -a "${MOFKA_RESULT_DIR}/stderr.log" >&2)

echo "Stopping Mofka fixed baseline in ${RESULT_DIR}"

if [[ -d "${MOFKA_RESULT_DIR}/pids" ]]; then
  for pid_file in "${MOFKA_RESULT_DIR}"/pids/mofka-storage-*.pid; do
    [[ -e "${pid_file}" ]] || continue
    stop_pid_file "$(basename "${pid_file}" .pid)" "${pid_file}" "${TIMEOUT_SECONDS}"
  done
  stop_pid_file "mofka-master" "${MOFKA_RESULT_DIR}/pids/mofka-master.pid" "${TIMEOUT_SECONDS}"
fi

echo "Mofka fixed baseline stop complete"
