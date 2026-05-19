#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.agent/scripts/kafka_common.sh
source "${SCRIPT_DIR}/kafka_common.sh"

usage() {
  cat <<'USAGE'
Usage: kafka_stop.sh [options]

Stop a Phase 0 Kafka fixed baseline launched by kafka_launch.sh.

Options:
  --result-dir DIR        Result directory containing kafka/pids.
  --timeout-seconds N     Graceful shutdown timeout per process. Default: 20.
  --clean-data            Remove Kafka data and ZooKeeper data under DIR/kafka.
  -h, --help              Show this help.
USAGE
}

find_latest_kafka_result_dir() {
  local latest
  latest="$(find "${PHASE0_REPO_ROOT}/.agent/results" -mindepth 1 -maxdepth 1 -type d \
    -exec test -d "{}/kafka/pids" \; -print 2>/dev/null | sort | tail -1)"
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
    echo "No ${label} pid file at ${pid_file}; skipping"
    return 0
  fi

  local pid
  pid="$(tr -d '[:space:]' < "${pid_file}")"
  if [[ -z "${pid}" ]]; then
    echo "Empty ${label} pid file at ${pid_file}; skipping"
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
TIMEOUT_SECONDS="${KAFKA_STOP_TIMEOUT_SECONDS:-20}"
CLEAN_DATA=0

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
    --clean-data)
      CLEAN_DATA=1
      shift
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
  RESULT_DIR="$(find_latest_kafka_result_dir)" || {
    echo "No Kafka result directory found; pass --result-dir" >&2
    exit 1
  }
fi

RESULT_DIR="$(cd "${RESULT_DIR}" && pwd)"
KAFKA_RESULT_DIR="${RESULT_DIR}/kafka"
KAFKA_PID_DIR="${KAFKA_RESULT_DIR}/pids"

mkdir -p "${KAFKA_RESULT_DIR}"
exec > >(tee -a "${KAFKA_RESULT_DIR}/stdout.log")
exec 2> >(tee -a "${KAFKA_RESULT_DIR}/stderr.log" >&2)

echo "Stopping Kafka fixed baseline in ${RESULT_DIR}"

shopt -s nullglob
broker_pid_files=("${KAFKA_PID_DIR}"/broker-*.pid)
if [[ "${#broker_pid_files[@]}" -gt 0 ]]; then
  for broker_pid_file in "${broker_pid_files[@]}"; do
    stop_pid_file "Kafka $(basename "${broker_pid_file}" .pid)" "${broker_pid_file}" "${TIMEOUT_SECONDS}"
  done
else
  stop_pid_file "Kafka broker" "${KAFKA_PID_DIR}/broker.pid" "${TIMEOUT_SECONDS}"
fi
shopt -u nullglob
stop_pid_file "ZooKeeper" "${KAFKA_PID_DIR}/zookeeper.pid" "${TIMEOUT_SECONDS}"

if [[ "${CLEAN_DATA}" -eq 1 ]]; then
  rm -rf "${KAFKA_RESULT_DIR}/data" "${KAFKA_RESULT_DIR}/zookeeper"
  echo "Removed Kafka runtime data under ${KAFKA_RESULT_DIR}"
fi

echo "Kafka fixed baseline stop complete"
