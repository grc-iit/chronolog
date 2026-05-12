#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.agent/scripts/kafka_common.sh
source "${SCRIPT_DIR}/kafka_common.sh"

usage() {
  cat <<'USAGE'
Usage: kafka_run_append_distributed.sh [options]

Run a two-node bare-metal Kafka append-throughput smoke benchmark.

Options:
  --result-dir DIR             Existing or new result directory to use.
  --partition NAME             SLURM partition. Default: debug.
  --node-count N               Node count. Default: 2.
  --slurm-nodelist LIST        Optional explicit SLURM nodelist.
  --slurm-time TIME            SLURM time limit. Default: 00:10:00.
  --operation-count N          Number of records. Default: 1000.
  --message-size-bytes N       Record size. Default: 1024.
  --topic NAME                 Topic name. Default: phase0-append.
  --zookeeper-port PORT        ZooKeeper port. Default: 22181.
  --broker-port PORT           Broker port. Default: 29092.
  -h, --help                   Show this help.
USAGE
}

RESULT_DIR="${PHASE0_RESULT_DIR:-}"
PARTITION="${KAFKA_SLURM_PARTITION:-debug}"
NODE_COUNT="${KAFKA_NODE_COUNT:-2}"
SLURM_NODELIST="${KAFKA_SLURM_NODELIST:-}"
SLURM_TIME="${KAFKA_SLURM_TIME:-00:10:00}"
OPERATION_COUNT="${KAFKA_OPERATION_COUNT:-1000}"
MESSAGE_SIZE_BYTES="${KAFKA_MESSAGE_SIZE_BYTES:-1024}"
TOPIC="${KAFKA_TOPIC:-phase0-append}"
ZOOKEEPER_PORT="${KAFKA_ZOOKEEPER_PORT:-22181}"
BROKER_PORT="${KAFKA_BROKER_PORT:-29092}"
KAFKA_HEAP_OPTS_VALUE="${KAFKA_HEAP_OPTS:-"-Xms128m -Xmx512m"}"
ZOOKEEPER_HEAP_OPTS_VALUE="${ZOOKEEPER_HEAP_OPTS:-"-Xms128m -Xmx512m"}"
NETWORK_IFACE="${KAFKA_NETWORK_IFACE:-enp47s0np0}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --result-dir) RESULT_DIR="$2"; shift 2 ;;
    --partition) PARTITION="$2"; shift 2 ;;
    --node-count) NODE_COUNT="$2"; shift 2 ;;
    --slurm-nodelist) SLURM_NODELIST="$2"; shift 2 ;;
    --slurm-time) SLURM_TIME="$2"; shift 2 ;;
    --operation-count) OPERATION_COUNT="$2"; shift 2 ;;
    --message-size-bytes) MESSAGE_SIZE_BYTES="$2"; shift 2 ;;
    --topic) TOPIC="$2"; shift 2 ;;
    --zookeeper-port) ZOOKEEPER_PORT="$2"; shift 2 ;;
    --broker-port) BROKER_PORT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ "${NODE_COUNT}" -lt 2 ]]; then
  echo "Distributed Kafka append requires at least 2 nodes" >&2
  exit 2
fi

RESULT_DIR="$(phase0_make_result_dir "${RESULT_DIR}")"
CONFIG_DIR="${RESULT_DIR}/config"
KAFKA_RESULT_DIR="${RESULT_DIR}/kafka"
KAFKA_LOG_DIR="${KAFKA_RESULT_DIR}/logs"
KAFKA_PID_DIR="${KAFKA_RESULT_DIR}/pids"
KAFKA_DATA_DIR="${KAFKA_RESULT_DIR}/data"
ZOOKEEPER_DATA_DIR="${KAFKA_RESULT_DIR}/zookeeper"
mkdir -p "${CONFIG_DIR}" "${KAFKA_RESULT_DIR}" "${KAFKA_LOG_DIR}" "${KAFKA_PID_DIR}" "${KAFKA_DATA_DIR}" "${ZOOKEEPER_DATA_DIR}"

exec > >(tee -a "${KAFKA_RESULT_DIR}/stdout.log")
exec 2> >(tee -a "${KAFKA_RESULT_DIR}/stderr.log" >&2)

KAFKA_HOME="$(phase0_install_kafka_if_needed)"

declare -a NODES=()
if [[ -n "${SLURM_NODELIST}" ]]; then
  if command -v scontrol >/dev/null 2>&1; then
    mapfile -t NODES < <(scontrol show hostnames "${SLURM_NODELIST}")
  else
    IFS=',' read -r -a NODES <<< "${SLURM_NODELIST}"
  fi
else
  mapfile -t NODES < <(sinfo -N -h -p "${PARTITION}" -t idle -o '%N' | sort -u | head -n "${NODE_COUNT}")
fi

if [[ "${#NODES[@]}" -lt "${NODE_COUNT}" ]]; then
  echo "Only found ${#NODES[@]} usable SLURM nodes for requested node count ${NODE_COUNT}" >&2
  exit 1
fi

ZOOKEEPER_NODE="${NODES[0]}"
BROKER_NODE="${NODES[1]}"
printf '%s\n' "${NODES[@]:0:${NODE_COUNT}}" > "${CONFIG_DIR}/kafka-slurm-nodes.txt"

node_ip() {
  local node="$1"
  srun --partition="${PARTITION}" --nodes=1 --ntasks=1 --time=00:02:00 --nodelist="${node}" \
    bash -lc "ip -4 -o addr show dev '${NETWORK_IFACE}' | cut -d' ' -f7 | cut -d/ -f1 | head -1 || hostname -I | cut -d' ' -f1" \
    2>> "${KAFKA_LOG_DIR}/ip-detect.stderr.log"
}

ZOOKEEPER_HOST="$(node_ip "${ZOOKEEPER_NODE}")"
BROKER_HOST="$(node_ip "${BROKER_NODE}")"
ZOOKEEPER_CONNECT="${ZOOKEEPER_HOST}:${ZOOKEEPER_PORT}"
BOOTSTRAP_SERVER="${BROKER_HOST}:${BROKER_PORT}"

ZOOKEEPER_CONFIG="${CONFIG_DIR}/kafka-zookeeper.properties"
SERVER_CONFIG="${CONFIG_DIR}/kafka-server.properties"

cat > "${ZOOKEEPER_CONFIG}" <<EOF
dataDir=${ZOOKEEPER_DATA_DIR}
clientPort=${ZOOKEEPER_PORT}
clientPortAddress=0.0.0.0
maxClientCnxns=0
admin.enableServer=false
EOF

cat > "${SERVER_CONFIG}" <<EOF
broker.id=0
listeners=PLAINTEXT://0.0.0.0:${BROKER_PORT}
advertised.listeners=PLAINTEXT://${BROKER_HOST}:${BROKER_PORT}
log.dirs=${KAFKA_DATA_DIR}
zookeeper.connect=${ZOOKEEPER_CONNECT}
offsets.topic.replication.factor=1
transaction.state.log.replication.factor=1
transaction.state.log.min.isr=1
num.partitions=1
auto.create.topics.enable=false
delete.topic.enable=true
EOF

cat > "${CONFIG_DIR}/kafka-config-manifest.env" <<EOF
deployment_mode=bare_metal
node_count=${NODE_COUNT}
client_count=1
workflow=append_throughput
message_size_bytes=${MESSAGE_SIZE_BYTES}
operation_count=${OPERATION_COUNT}
partition=${PARTITION}
slurm_time=${SLURM_TIME}
zookeeper_node=${ZOOKEEPER_NODE}
broker_node=${BROKER_NODE}
network_iface=${NETWORK_IFACE}
zookeeper_connect=${ZOOKEEPER_CONNECT}
bootstrap_server=${BOOTSTRAP_SERVER}
kafka_home=${KAFKA_HOME}
kafka_server_config=${SERVER_CONFIG}
kafka_zookeeper_config=${ZOOKEEPER_CONFIG}
kafka_heap_opts=${KAFKA_HEAP_OPTS_VALUE}
zookeeper_heap_opts=${ZOOKEEPER_HEAP_OPTS_VALUE}
EOF

cleanup() {
  set +e
  "${SCRIPT_DIR}/kafka_stop.sh" --result-dir "${RESULT_DIR}" >/dev/null 2>&1
}
trap cleanup EXIT

echo "Kafka distributed result directory: ${RESULT_DIR}"
echo "ZooKeeper node: ${ZOOKEEPER_NODE} (${ZOOKEEPER_HOST})"
echo "Broker node: ${BROKER_NODE} (${BROKER_HOST})"
echo "Bootstrap server: ${BOOTSTRAP_SERVER}"

srun --partition="${PARTITION}" --nodes=1 --ntasks=1 --exclusive \
  --time="${SLURM_TIME}" --nodelist="${ZOOKEEPER_NODE}" \
  bash -lc "hostname; export KAFKA_HEAP_OPTS='${ZOOKEEPER_HEAP_OPTS_VALUE}'; exec '${KAFKA_HOME}/bin/zookeeper-server-start.sh' '${ZOOKEEPER_CONFIG}'" \
  > "${KAFKA_LOG_DIR}/zookeeper.stdout.log" \
  2> "${KAFKA_LOG_DIR}/zookeeper.stderr.log" &
echo "$!" > "${KAFKA_PID_DIR}/zookeeper.pid"

if ! phase0_wait_for_port "${ZOOKEEPER_HOST}" "${ZOOKEEPER_PORT}" 60; then
  echo "ZooKeeper failed to listen on ${ZOOKEEPER_CONNECT}" >&2
  exit 1
fi

srun --partition="${PARTITION}" --nodes=1 --ntasks=1 --exclusive \
  --time="${SLURM_TIME}" --nodelist="${BROKER_NODE}" \
  bash -lc "hostname; export KAFKA_HEAP_OPTS='${KAFKA_HEAP_OPTS_VALUE}'; exec '${KAFKA_HOME}/bin/kafka-server-start.sh' '${SERVER_CONFIG}'" \
  > "${KAFKA_LOG_DIR}/broker.stdout.log" \
  2> "${KAFKA_LOG_DIR}/broker.stderr.log" &
echo "$!" > "${KAFKA_PID_DIR}/broker.pid"

if ! phase0_wait_for_port "${BROKER_HOST}" "${BROKER_PORT}" 60; then
  echo "Kafka broker failed to listen on ${BOOTSTRAP_SERVER}" >&2
  exit 1
fi

ADMIN_DEADLINE=$((SECONDS + 60))
until "${KAFKA_HOME}/bin/kafka-topics.sh" --bootstrap-server "${BOOTSTRAP_SERVER}" --list >/dev/null; do
  if (( SECONDS >= ADMIN_DEADLINE )); then
    echo "Kafka broker port opened, but admin client did not become ready for ${BOOTSTRAP_SERVER}" >&2
    exit 1
  fi
  sleep 1
done

"${KAFKA_HOME}/bin/kafka-topics.sh" --bootstrap-server "${BOOTSTRAP_SERVER}" \
  --create --if-not-exists --topic "${TOPIC}" --partitions 1 --replication-factor 1 \
  > "${KAFKA_RESULT_DIR}/topic-create.log" 2>&1

"${KAFKA_HOME}/bin/kafka-producer-perf-test.sh" \
  --topic "${TOPIC}" \
  --num-records "${OPERATION_COUNT}" \
  --record-size "${MESSAGE_SIZE_BYTES}" \
  --throughput -1 \
  --producer-props "bootstrap.servers=${BOOTSTRAP_SERVER}" acks=1 \
  > "${KAFKA_RESULT_DIR}/producer-perf-append-throughput.log" \
  2> "${KAFKA_RESULT_DIR}/producer-perf-append-throughput.stderr.log"

python3 - "$KAFKA_RESULT_DIR/producer-perf-append-throughput.log" "${KAFKA_RESULT_DIR}/metrics.json" "${NODE_COUNT}" "${OPERATION_COUNT}" "${MESSAGE_SIZE_BYTES}" <<'PY'
import json
import re
import sys

log_path, metrics_path, node_count, operation_count, message_size = sys.argv[1:]
text = open(log_path).read()
pattern = re.compile(
    r"(?P<records>\d+) records sent, (?P<tput>[0-9.]+) records/sec .*?, "
    r"(?P<avg>[0-9.]+) ms avg latency, .*?, (?P<p50>[0-9.]+) ms 50th, "
    r"(?P<p95>[0-9.]+) ms 95th, (?P<p99>[0-9.]+) ms 99th"
)
match = pattern.search(text)
if not match:
    raise SystemExit(f"Could not parse producer perf output: {text}")
records = int(match.group("records"))
throughput = float(match.group("tput"))
duration = records / throughput if throughput else 0
metrics = {
    "system": "kafka",
    "workflow": "append_throughput",
    "node_count": int(node_count),
    "client_count": 1,
    "message_size_bytes": int(message_size),
    "operation_count": int(operation_count),
    "duration_seconds": duration,
    "throughput_ops_per_sec": throughput,
    "avg_latency_ms": float(match.group("avg")),
    "p50_latency_ms": float(match.group("p50")),
    "p95_latency_ms": float(match.group("p95")),
    "p99_latency_ms": float(match.group("p99")),
    "success": True,
}
open(metrics_path, "w").write(json.dumps(metrics, indent=2) + "\n")
print(json.dumps(metrics, indent=2))
PY

cat > "${RESULT_DIR}/summary.md" <<EOF
# Kafka Distributed Append Smoke

- system: Kafka
- workflow: append_throughput
- deployment_mode: bare_metal
- node_count: ${NODE_COUNT}
- zookeeper_node: ${ZOOKEEPER_NODE}
- broker_node: ${BROKER_NODE}
- bootstrap_server: ${BOOTSTRAP_SERVER}
- operation_count: ${OPERATION_COUNT}
- message_size_bytes: ${MESSAGE_SIZE_BYTES}
- metrics: kafka/metrics.json
EOF
