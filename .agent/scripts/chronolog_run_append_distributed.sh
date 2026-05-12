#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

usage() {
  cat <<'USAGE'
Usage: chronolog_run_append_distributed.sh [options]

Run a two-node bare-metal ChronoLog append-throughput smoke benchmark.

Options:
  --result-dir DIR             Existing or new result directory to use.
  --partition NAME             SLURM partition. Default: debug.
  --node-count N               Node count. Default: 2.
  --slurm-time TIME            SLURM time limit. Default: 00:10:00.
  --record-groups N            ChronoLog recording groups. Default: 1.
  --workflow NAME              append_throughput, append_latency, or range_retrieval. Default: append_throughput.
  --operation-count N          Events per client. Default: 10.
  --message-size-bytes N       Average event size. Default: 1024.
  --chronicle-count N          Chronicles per process. Default: 1.
  --story-count N              Stories per process. Default: 1.
  -h, --help                   Show this help.
USAGE
}

timestamp() {
  date +%Y%m%d-%H%M%S
}

make_result_dir() {
  local requested="$1"
  if [[ -z "${requested}" ]]; then
    requested="${REPO_ROOT}/.agent/results/$(timestamp)"
  fi
  mkdir -p "${requested}"
  cd "${requested}" && pwd
}

module_loads() {
  cat <<'EOF'
module load gcc/11.4.0/cmake/3.30.5-pq5ntgo
module load gcc/11.4.0/argobots/1.1-indjy5q
module load gcc/11.4.0/libfabric/1.22.0-lyviqpl
module load gcc/11.4.0/mercury/2.3.1-kcmn6u3
module load gcc/11.4.0/mochi-margo/0.17.0-llh62si
module load gcc/11.4.0/mochi-thallium/0.10.1-2pari3f
module load gcc/11.4.0/boost/1.86.0-cs4onpk
module load gcc/11.4.0/spdlog/1.14.1-y4quhau
module load gcc/11.4.0/json-c/0.16-2ofsb6t
module load gcc/11.4.0/pkgconf/2.2.0-3sczowe
module load gcc/11.4.0/python/3.11.9-zg4555e
module load gcc/11.4.0/py-pybind11/2.13.5-pngixx5
module load openmpi/5.0.5-x2kvx5l/gcc/11.4.0/hdf5/1.14.5-5zbyeqh
EOF
}

RESULT_DIR="${PHASE0_RESULT_DIR:-}"
PARTITION="${CHRONOLOG_SLURM_PARTITION:-debug}"
NODE_COUNT="${CHRONOLOG_NODE_COUNT:-2}"
SLURM_TIME="${CHRONOLOG_SLURM_TIME:-00:10:00}"
RECORD_GROUPS="${CHRONOLOG_RECORD_GROUPS:-1}"
OPERATION_COUNT="${CHRONOLOG_OPERATION_COUNT:-10}"
MESSAGE_SIZE_BYTES="${CHRONOLOG_MESSAGE_SIZE_BYTES:-1024}"
CHRONICLE_COUNT="${CHRONOLOG_CHRONICLE_COUNT:-1}"
STORY_COUNT="${CHRONOLOG_STORY_COUNT:-1}"
INSIDE_ALLOCATION="${CHRONOLOG_INSIDE_ALLOCATION:-0}"
WORKFLOW="${CHRONOLOG_WORKFLOW:-append_throughput}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --result-dir) RESULT_DIR="$2"; shift 2 ;;
    --partition) PARTITION="$2"; shift 2 ;;
    --node-count) NODE_COUNT="$2"; shift 2 ;;
    --slurm-time) SLURM_TIME="$2"; shift 2 ;;
    --record-groups) RECORD_GROUPS="$2"; shift 2 ;;
    --workflow) WORKFLOW="$2"; shift 2 ;;
    --operation-count) OPERATION_COUNT="$2"; shift 2 ;;
    --message-size-bytes) MESSAGE_SIZE_BYTES="$2"; shift 2 ;;
    --chronicle-count) CHRONICLE_COUNT="$2"; shift 2 ;;
    --story-count) STORY_COUNT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ "${NODE_COUNT}" -lt 2 ]]; then
  echo "Distributed ChronoLog append requires at least 2 nodes" >&2
  exit 2
fi

RESULT_DIR="$(make_result_dir "${RESULT_DIR}")"
export PHASE0_RESULT_DIR="${RESULT_DIR}"

if [[ -z "${SLURM_JOB_ID:-}" && "${INSIDE_ALLOCATION}" != "1" ]]; then
  exec salloc \
    --partition="${PARTITION}" \
    --nodes="${NODE_COUNT}" \
    --time="${SLURM_TIME}" \
    --job-name=phase0-chronolog \
    /usr/bin/env CHRONOLOG_INSIDE_ALLOCATION=1 PHASE0_RESULT_DIR="${RESULT_DIR}" "$0" \
      --partition "${PARTITION}" \
      --node-count "${NODE_COUNT}" \
      --slurm-time "${SLURM_TIME}" \
      --record-groups "${RECORD_GROUPS}" \
      --workflow "${WORKFLOW}" \
      --operation-count "${OPERATION_COUNT}" \
      --message-size-bytes "${MESSAGE_SIZE_BYTES}" \
      --chronicle-count "${CHRONICLE_COUNT}" \
      --story-count "${STORY_COUNT}"
fi

CONFIG_DIR="${RESULT_DIR}/config"
CHRONOLOG_RESULT_DIR="${RESULT_DIR}/chronolog"
CHRONOLOG_LOG_DIR="${CHRONOLOG_RESULT_DIR}/logs"
CHRONOLOG_OUTPUT_DIR="${CHRONOLOG_RESULT_DIR}/output"
WRAPPER_DIR="${CHRONOLOG_RESULT_DIR}/wrappers"
DEPLOY_WORK_DIR="${CHRONOLOG_RESULT_DIR}/deploy-work"
DEPLOY_CONF_DIR="${DEPLOY_WORK_DIR}/conf"
mkdir -p "${CONFIG_DIR}" "${CHRONOLOG_RESULT_DIR}" "${CHRONOLOG_LOG_DIR}" "${CHRONOLOG_OUTPUT_DIR}" "${WRAPPER_DIR}" "${DEPLOY_CONF_DIR}"

exec > >(tee -a "${CHRONOLOG_RESULT_DIR}/stdout.log")
exec 2> >(tee -a "${CHRONOLOG_RESULT_DIR}/stderr.log" >&2)

INSTALL_DIR="${REPO_ROOT}/.agent/install-consistent/chronolog"
DEPLOY_SCRIPT="${INSTALL_DIR}/tools/deploy/deploy_cluster.sh"
BENCH_BIN="${INSTALL_DIR}/tools/benchmark/chrono-bench"
BASE_CONF="${INSTALL_DIR}/conf/default-chrono-conf.json"
BASE_CLIENT_CONF="${INSTALL_DIR}/conf/default-chrono-client-conf.json"
CONF_FILE="${CONFIG_DIR}/default-chrono-conf.json"
CLIENT_CONF_FILE="${CONFIG_DIR}/default-chrono-client-conf.json"

cp "${BASE_CONF}" "${CONF_FILE}"
cp "${BASE_CLIENT_CONF}" "${CLIENT_CONF_FILE}"

if [[ -n "${SLURM_JOB_NODELIST:-}" ]]; then
  scontrol show hostnames "${SLURM_JOB_NODELIST}" | head -n "${NODE_COUNT}" > "${CONFIG_DIR}/chronolog-slurm-nodes.txt"
else
  sinfo -N -h -p "${PARTITION}" -t idle -o '%N' | sort -u | head -n "${NODE_COUNT}" > "${CONFIG_DIR}/chronolog-slurm-nodes.txt"
fi

if [[ "$(wc -l < "${CONFIG_DIR}/chronolog-slurm-nodes.txt")" -lt "${NODE_COUNT}" ]]; then
  echo "Only found $(wc -l < "${CONFIG_DIR}/chronolog-slurm-nodes.txt") nodes for requested node count ${NODE_COUNT}" >&2
  exit 1
fi

head -n 1 "${CONFIG_DIR}/chronolog-slurm-nodes.txt" > "${CONFIG_DIR}/hosts_visor"
cp "${CONFIG_DIR}/chronolog-slurm-nodes.txt" "${CONFIG_DIR}/hosts_keeper"
tail -n "${RECORD_GROUPS}" "${CONFIG_DIR}/chronolog-slurm-nodes.txt" > "${CONFIG_DIR}/hosts_grapher"
tail -n "${RECORD_GROUPS}" "${CONFIG_DIR}/chronolog-slurm-nodes.txt" > "${CONFIG_DIR}/hosts_player"
cp "${CONFIG_DIR}/hosts_player" "${DEPLOY_CONF_DIR}/hosts_player"
CLIENT_NODE="$(head -n 1 "${CONFIG_DIR}/chronolog-slurm-nodes.txt")"
CLIENT_IP="$(dig +short "${CLIENT_NODE}-40g" | head -n 1)"
if [[ -z "${CLIENT_IP}" ]]; then
  CLIENT_IP="$(dig +short "${CLIENT_NODE}" | head -n 1)"
fi
if [[ -z "${CLIENT_IP}" ]]; then
  echo "Could not resolve client node IP for ${CLIENT_NODE}" >&2
  exit 1
fi

create_wrapper() {
  local role="$1"
  local real_bin="${INSTALL_DIR}/bin/${role}"
  local wrapper="${WRAPPER_DIR}/${role}"
  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf '%s\n' 'source /etc/profile.d/lmod.sh 2>/dev/null || source /etc/profile.d/modules.sh 2>/dev/null || true'
    printf '%s\n' 'type module >/dev/null 2>&1 || { echo "module command unavailable on $(hostname)" >&2; exit 127; }'
    module_loads
    printf 'real_bin=%q\n' "${real_bin}"
    printf '%s\n' '"${real_bin}" "$@" &'
    printf '%s\n' 'child=$!'
    printf '%s\n' 'term_child() { kill -TERM "${child}" 2>/dev/null || true; wait "${child}" 2>/dev/null || true; }'
    printf '%s\n' 'trap term_child TERM INT'
    printf '%s\n' 'wait "${child}"'
  } > "${wrapper}"
  chmod +x "${wrapper}"
}

create_wrapper chrono-visor
create_wrapper chrono-grapher
create_wrapper chrono-keeper
create_wrapper chrono-player

cat > "${CONFIG_DIR}/chronolog-config-manifest.env" <<EOF
deployment_mode=bare_metal
node_count=${NODE_COUNT}
client_count=1
workflow=${WORKFLOW}
message_size_bytes=${MESSAGE_SIZE_BYTES}
operation_count=${OPERATION_COUNT}
partition=${PARTITION}
slurm_job_id=${SLURM_JOB_ID:-}
slurm_time=${SLURM_TIME}
record_groups=${RECORD_GROUPS}
chronolog_install=${INSTALL_DIR}
chronolog_config=${CONF_FILE}
chronolog_client_config=${CLIENT_CONF_FILE}
chronolog_deploy_script=${DEPLOY_SCRIPT}
chronolog_benchmark=${BENCH_BIN}
chronolog_deploy_work_dir=${DEPLOY_WORK_DIR}
client_node=${CLIENT_NODE}
client_ip=${CLIENT_IP}
transport=ofi+sockets
EOF

cleanup() {
  set +e
  "${DEPLOY_SCRIPT}" --stop \
    --work-dir "${DEPLOY_WORK_DIR}" \
    --visor-hosts "${CONFIG_DIR}/hosts_visor" \
    --keeper-hosts "${CONFIG_DIR}/hosts_keeper" \
    --grapher-hosts "${CONFIG_DIR}/hosts_grapher" \
    --conf-file "${CONF_FILE}" \
    --client-conf-file "${CLIENT_CONF_FILE}" \
    --visor-bin "${WRAPPER_DIR}/chrono-visor" \
    --keeper-bin "${WRAPPER_DIR}/chrono-keeper" \
    --grapher-bin "${WRAPPER_DIR}/chrono-grapher" \
    --player-bin "${WRAPPER_DIR}/chrono-player" \
    > "${CHRONOLOG_LOG_DIR}/deploy-stop.log" 2>&1
}
trap cleanup EXIT

echo "ChronoLog distributed result directory: ${RESULT_DIR}"
echo "SLURM job: ${SLURM_JOB_ID:-none}"
echo "Nodes:"
cat "${CONFIG_DIR}/chronolog-slurm-nodes.txt"

"${DEPLOY_SCRIPT}" --start \
  --work-dir "${DEPLOY_WORK_DIR}" \
  --record-groups "${RECORD_GROUPS}" \
  --visor-hosts "${CONFIG_DIR}/hosts_visor" \
  --keeper-hosts "${CONFIG_DIR}/hosts_keeper" \
  --grapher-hosts "${CONFIG_DIR}/hosts_grapher" \
  --conf-file "${CONF_FILE}" \
  --client-conf-file "${CLIENT_CONF_FILE}" \
  --monitor-dir "${CHRONOLOG_LOG_DIR}" \
  --output-dir "${CHRONOLOG_OUTPUT_DIR}" \
  --visor-bin "${WRAPPER_DIR}/chrono-visor" \
  --keeper-bin "${WRAPPER_DIR}/chrono-keeper" \
  --grapher-bin "${WRAPPER_DIR}/chrono-grapher" \
  --player-bin "${WRAPPER_DIR}/chrono-player"

jq ".chrono_client.ClientQueryService.rpc.service_ip = \"${CLIENT_IP}\"" "${CLIENT_CONF_FILE}" > "${CONFIG_DIR}/client-conf.tmp" \
  && mv "${CONFIG_DIR}/client-conf.tmp" "${CLIENT_CONF_FILE}"

sleep 5

MIN_EVENT_SIZE=$((MESSAGE_SIZE_BYTES / 2))
MAX_EVENT_SIZE=$((MESSAGE_SIZE_BYTES * 2))

if [[ "${WORKFLOW}" == "append_throughput" ]]; then
  set +e
  bash -lc "$(module_loads); mpirun -n 1 '${BENCH_BIN}' -c '${CLIENT_CONF_FILE}' -w -h '${CHRONICLE_COUNT}' -t '${STORY_COUNT}' -a '${MIN_EVENT_SIZE}' -s '${MESSAGE_SIZE_BYTES}' -b '${MAX_EVENT_SIZE}' -n '${OPERATION_COUNT}' -p" \
    > "${CHRONOLOG_RESULT_DIR}/chrono-bench-append-throughput.log" \
    2> "${CHRONOLOG_RESULT_DIR}/chrono-bench-append-throughput.stderr.log"
  BENCH_STATUS=$?
  set -e

  python3 - "${CHRONOLOG_RESULT_DIR}/chrono-bench-append-throughput.log" "${CHRONOLOG_RESULT_DIR}/metrics.json" "${NODE_COUNT}" "${OPERATION_COUNT}" "${MESSAGE_SIZE_BYTES}" "${BENCH_STATUS}" <<'PY'
import json
import re
import sys

log_path, metrics_path, node_count, operation_count, message_size, status = sys.argv[1:]
text = open(log_path, encoding="utf-8", errors="replace").read()
throughput_match = re.search(r"Record-event \(incl\. metadata time\) throughput:\s+([0-9.eE+-]+)\s+events/s", text)
bandwidth_match = re.search(r"Record-event \(incl\. metadata time\) bandwidth:\s+([0-9.eE+-]+)\s+MB/s", text)
throughput = float(throughput_match.group(1)) if throughput_match else 0.0
duration = int(operation_count) / throughput if throughput else 0.0
metrics = {
    "system": "chronolog",
    "workflow": "append_throughput",
    "node_count": int(node_count),
    "client_count": 1,
    "message_size_bytes": int(message_size),
    "operation_count": int(operation_count),
    "duration_seconds": duration,
    "throughput_ops_per_sec": throughput,
    "avg_latency_ms": None,
    "p50_latency_ms": None,
    "p95_latency_ms": None,
    "p99_latency_ms": None,
    "success": int(status) == 0 and bool(throughput_match),
}
if bandwidth_match:
    metrics["record_event_bandwidth_mb_per_sec"] = float(bandwidth_match.group(1))
open(metrics_path, "w").write(json.dumps(metrics, indent=2) + "\n")
print(json.dumps(metrics, indent=2))
if not metrics["success"]:
    raise SystemExit(1)
PY
elif [[ "${WORKFLOW}" == "append_latency" ]]; then
  set +e
  bash -lc "$(module_loads); export LD_LIBRARY_PATH='${INSTALL_DIR}/lib':\${LD_LIBRARY_PATH:-}; export PYTHONPATH='${INSTALL_DIR}/lib':\${PYTHONPATH:-}; python3 '${SCRIPT_DIR}/chronolog_append_latency.py' --config '${CLIENT_CONF_FILE}' --result-dir '${RESULT_DIR}' --operation-count '${OPERATION_COUNT}' --message-size-bytes '${MESSAGE_SIZE_BYTES}' --node-count '${NODE_COUNT}' --client-count 1 --workflow append_latency" \
    > "${CHRONOLOG_RESULT_DIR}/chronolog-append-latency.log" \
    2> "${CHRONOLOG_RESULT_DIR}/chronolog-append-latency.stderr.log"
  BENCH_STATUS=$?
  set -e
  if [[ "${BENCH_STATUS}" -ne 0 ]]; then
    exit "${BENCH_STATUS}"
  fi
elif [[ "${WORKFLOW}" == "range_retrieval" ]]; then
  CLIENT_COMMAND="${CONFIG_DIR}/chronolog-range-client.sh"
  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf 'cd %q\n' "${REPO_ROOT}"
    printf '%s\n' 'source /etc/profile.d/lmod.sh 2>/dev/null || source /etc/profile.d/modules.sh 2>/dev/null || true'
    printf '%s\n' 'type module >/dev/null 2>&1 || { echo "module command unavailable on $(hostname)" >&2; exit 127; }'
    module_loads
    printf 'export LD_LIBRARY_PATH=%q:${LD_LIBRARY_PATH:-}\n' "${INSTALL_DIR}/lib"
    printf 'export PYTHONPATH=%q:${PYTHONPATH:-}\n' "${INSTALL_DIR}/lib"
    printf 'timeout 600s python3 %q --config %q --result-dir %q --operation-count %q --message-size-bytes %q --node-count %q --client-count 1\n' \
      "${SCRIPT_DIR}/chronolog_range_retrieval.py" \
      "${CLIENT_CONF_FILE}" \
      "${RESULT_DIR}" \
      "${OPERATION_COUNT}" \
      "${MESSAGE_SIZE_BYTES}" \
      "${NODE_COUNT}"
  } > "${CLIENT_COMMAND}"
  chmod +x "${CLIENT_COMMAND}"
  set +e
  ssh -n "${CLIENT_NODE}" "bash '${CLIENT_COMMAND}'" \
    > "${CHRONOLOG_RESULT_DIR}/chronolog-range-retrieval.log" \
    2> "${CHRONOLOG_RESULT_DIR}/chronolog-range-retrieval.stderr.log"
  BENCH_STATUS=$?
  set -e
  if [[ "${BENCH_STATUS}" -ne 0 ]]; then
    exit "${BENCH_STATUS}"
  fi
else
  echo "Unsupported ChronoLog workflow: ${WORKFLOW}" >&2
  exit 2
fi

cat > "${RESULT_DIR}/summary.md" <<EOF
# ChronoLog Distributed Append Smoke

- system: ChronoLog
- workflow: ${WORKFLOW}
- deployment_mode: bare_metal
- transport: ofi+sockets
- node_count: ${NODE_COUNT}
- record_groups: ${RECORD_GROUPS}
- operation_count: ${OPERATION_COUNT}
- message_size_bytes: ${MESSAGE_SIZE_BYTES}
- metrics: chronolog/metrics.json
EOF
