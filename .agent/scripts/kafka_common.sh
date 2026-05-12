#!/usr/bin/env bash

set -euo pipefail

PHASE0_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

KAFKA_VERSION="${KAFKA_VERSION:-2.8.0}"
KAFKA_SCALA_VERSION="${KAFKA_SCALA_VERSION:-2.13}"
KAFKA_DIST_NAME="kafka_${KAFKA_SCALA_VERSION}-${KAFKA_VERSION}"
KAFKA_PREFIX="${KAFKA_PREFIX:-${PHASE0_REPO_ROOT}/opt/kafka}"
KAFKA_ARCHIVE_URL="${KAFKA_ARCHIVE_URL:-https://archive.apache.org/dist/kafka/${KAFKA_VERSION}/${KAFKA_DIST_NAME}.tgz}"

phase0_timestamp() {
  date +%Y%m%d-%H%M%S
}

phase0_make_result_dir() {
  local requested="${1:-}"
  if [[ -n "${requested}" ]]; then
    mkdir -p "${requested}"
    cd "${requested}" && pwd
    return
  fi

  local result_dir="${PHASE0_REPO_ROOT}/.agent/results/$(phase0_timestamp)"
  mkdir -p "${result_dir}"
  echo "${result_dir}"
}

phase0_require_java() {
  if ! command -v java >/dev/null 2>&1; then
    echo "java is required to launch Kafka; no java command was found on PATH" >&2
    return 1
  fi
}

phase0_detect_kafka_home() {
  if [[ -n "${KAFKA_HOME:-}" && -x "${KAFKA_HOME}/bin/kafka-server-start.sh" ]]; then
    echo "${KAFKA_HOME}"
    return 0
  fi

  local default_home="${KAFKA_PREFIX}/${KAFKA_DIST_NAME}"
  if [[ -x "${default_home}/bin/kafka-server-start.sh" ]]; then
    echo "${default_home}"
    return 0
  fi

  if command -v kafka-server-start.sh >/dev/null 2>&1; then
    local server_start
    server_start="$(command -v kafka-server-start.sh)"
    cd "$(dirname "${server_start}")/.." && pwd
    return 0
  fi

  return 1
}

phase0_install_kafka_if_needed() {
  phase0_require_java

  if phase0_detect_kafka_home >/dev/null 2>&1; then
    phase0_detect_kafka_home
    return 0
  fi

  mkdir -p "${KAFKA_PREFIX}/downloads"
  local archive="${KAFKA_PREFIX}/downloads/${KAFKA_DIST_NAME}.tgz"

  if [[ ! -s "${archive}" ]]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L --fail --retry 3 -o "${archive}" "${KAFKA_ARCHIVE_URL}"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "${archive}" "${KAFKA_ARCHIVE_URL}"
    else
      echo "Kafka is not installed and neither curl nor wget is available for project-local install" >&2
      return 1
    fi
  fi

  tar -xzf "${archive}" -C "${KAFKA_PREFIX}"
  phase0_detect_kafka_home
}

phase0_wait_for_port() {
  local host="$1"
  local port="$2"
  local timeout_seconds="$3"
  local deadline=$((SECONDS + timeout_seconds))

  while (( SECONDS < deadline )); do
    if (exec 3<>"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
      exec 3<&-
      exec 3>&-
      return 0
    fi
    sleep 1
  done

  return 1
}
