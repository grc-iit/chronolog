#!/usr/bin/env bash

set -euo pipefail

PHASE0_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

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

mofka_command_summary() {
  {
    command -v bedrock || true
    command -v mofkactl || true
    command -v python3 || true
  } | sed '/^$/d'
}

mofka_require_commands() {
  local missing=0
  for command_name in bedrock mofkactl; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
      echo "Missing required Mofka command: ${command_name}" >&2
      missing=1
    fi
  done
  return "${missing}"
}

mofka_wait_for_file() {
  local path="$1"
  local timeout_seconds="$2"
  local deadline=$((SECONDS + timeout_seconds))

  while (( SECONDS < deadline )); do
    if [[ -s "${path}" ]]; then
      return 0
    fi
    sleep 1
  done

  return 1
}
