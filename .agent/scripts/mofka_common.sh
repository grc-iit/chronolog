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

mofka_prepend_path() {
  local var_name="$1"
  local entry="$2"
  local current_value="${!var_name:-}"

  [[ -d "${entry}" ]] || return 0

  case ":${current_value}:" in
    *":${entry}:"*) ;;
    *)
      if [[ -n "${current_value}" ]]; then
        export "${var_name}=${entry}:${current_value}"
      else
        export "${var_name}=${entry}"
      fi
      ;;
  esac
}

mofka_export_spack_runtime_env() {
  local spec="${MOFKA_SPACK_SPEC:-mofka@0.6.4+python~mpi~benchmark~kafka}"

  if ! command -v spack >/dev/null 2>&1; then
    return 0
  fi

  if ! command -v bedrock >/dev/null 2>&1 || ! command -v mofkactl >/dev/null 2>&1; then
    eval "$(spack load --sh "${spec}")"
  fi

  local prefix
  while IFS= read -r prefix; do
    [[ -n "${prefix}" ]] || continue
    mofka_prepend_path PATH "${prefix}/bin"
    mofka_prepend_path LD_LIBRARY_PATH "${prefix}/lib"
    mofka_prepend_path LD_LIBRARY_PATH "${prefix}/lib64"
    mofka_prepend_path PYTHONPATH "${prefix}/lib/python3.11/site-packages"
  done < <(spack find -dp "${spec}" | awk '$NF ~ /^\// {print $NF}')
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
