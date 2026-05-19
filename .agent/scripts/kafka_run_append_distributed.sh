#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.agent/scripts/kafka_common.sh
source "${SCRIPT_DIR}/kafka_common.sh"

reject_tmpfs_storage_path() {
  local label="$1"
  local path="$2"
  local resolved
  resolved="$(realpath -m "${path}")"
  case "${resolved}" in
    /tmp|/tmp/*|/dev/shm|/dev/shm/*)
      echo "${label} must not be tmpfs-backed (${resolved}); Kafka log.dirs durability evidence requires persistent/shared storage or explicit local NVMe/SSD" >&2
      exit 2
      ;;
  esac
  if command -v findmnt >/dev/null 2>&1; then
    local fs_type
    local mount_probe="${resolved}"
    while [[ ! -e "${mount_probe}" && "${mount_probe}" != "/" ]]; do
      mount_probe="$(dirname "${mount_probe}")"
    done
    fs_type="$(findmnt -T "${mount_probe}" -no FSTYPE 2>/dev/null | head -n 1 || true)"
    case "${fs_type}" in
      tmpfs|ramfs|devtmpfs)
        echo "${label} resolves to ${fs_type} (${resolved}); Kafka storage benchmarks must not use memory-backed filesystems" >&2
        exit 2
        ;;
    esac
  fi
}

usage() {
  cat <<'USAGE'
Usage: kafka_run_append_distributed.sh [options]

Run a two-node bare-metal Kafka append-throughput benchmark.

Options:
  --result-dir DIR             Existing or new result directory to use.
  --partition NAME             SLURM partition. Default: debug.
  --node-count N               Node count. Default: 2.
  --client-count N             Parallel producer process count. Default: 1.
  --slurm-nodelist LIST        Optional explicit SLURM nodelist.
  --slurm-time TIME            SLURM time limit. Default: 00:10:00.
  --operation-count N          Number of records. Default: 1000.
  --message-size-bytes N       Record size. Default: 1024.
  --workflow NAME              append_throughput, append_latency, or range_retrieval. Default: append_throughput.
  --topic NAME                 Topic name. Default: phase0-append.
  --acks VALUE                 Producer acks value. Default: 1.
  --zookeeper-port PORT        ZooKeeper port. Default: 22181.
  --broker-port PORT           Broker port. Default: 29092.
  -h, --help                   Show this help.
USAGE
}

RESULT_DIR="${PHASE0_RESULT_DIR:-}"
PARTITION="${KAFKA_SLURM_PARTITION:-debug}"
NODE_COUNT="${KAFKA_NODE_COUNT:-2}"
CLIENT_COUNT="${KAFKA_CLIENT_COUNT:-1}"
SLURM_NODELIST="${KAFKA_SLURM_NODELIST:-}"
SLURM_TIME="${KAFKA_SLURM_TIME:-00:10:00}"
OPERATION_COUNT="${KAFKA_OPERATION_COUNT:-1000}"
MESSAGE_SIZE_BYTES="${KAFKA_MESSAGE_SIZE_BYTES:-1024}"
TOPIC="${KAFKA_TOPIC:-phase0-append}"
WORKFLOW="${KAFKA_WORKFLOW:-append_throughput}"
KAFKA_ACKS="${KAFKA_ACKS:-1}"
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
    --client-count) CLIENT_COUNT="$2"; shift 2 ;;
    --slurm-nodelist) SLURM_NODELIST="$2"; shift 2 ;;
    --slurm-time) SLURM_TIME="$2"; shift 2 ;;
    --operation-count) OPERATION_COUNT="$2"; shift 2 ;;
    --message-size-bytes) MESSAGE_SIZE_BYTES="$2"; shift 2 ;;
    --workflow) WORKFLOW="$2"; shift 2 ;;
    --topic) TOPIC="$2"; shift 2 ;;
    --acks) KAFKA_ACKS="$2"; shift 2 ;;
    --zookeeper-port) ZOOKEEPER_PORT="$2"; shift 2 ;;
    --broker-port) BROKER_PORT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

case "${KAFKA_ACKS}" in
  0|1|all|-1) ;;
  *)
    echo "Unsupported Kafka --acks value '${KAFKA_ACKS}'. Expected one of: 0, 1, all, -1." >&2
    exit 2
    ;;
esac

if [[ "${NODE_COUNT}" -lt 2 ]]; then
  echo "Distributed Kafka append requires at least 2 nodes" >&2
  exit 2
fi
if [[ "${CLIENT_COUNT}" -lt 1 ]]; then
  echo "Kafka client-count must be >= 1" >&2
  exit 2
fi

RESULT_DIR="$(phase0_make_result_dir "${RESULT_DIR}")"
reject_tmpfs_storage_path "--result-dir" "${RESULT_DIR}"
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
BROKER_NODES=("${NODES[@]:1:$((NODE_COUNT - 1))}")
printf '%s\n' "${NODES[@]:0:${NODE_COUNT}}" > "${CONFIG_DIR}/kafka-slurm-nodes.txt"

node_ip() {
  local node="$1"
  srun --partition="${PARTITION}" --nodes=1 --ntasks=1 --time=00:02:00 --nodelist="${node}" \
    bash -lc "ip -4 -o addr show dev '${NETWORK_IFACE}' | cut -d' ' -f7 | cut -d/ -f1 | head -1 || hostname -I | cut -d' ' -f1" \
    2>> "${KAFKA_LOG_DIR}/ip-detect.stderr.log"
}

ZOOKEEPER_HOST="$(node_ip "${ZOOKEEPER_NODE}")"
ZOOKEEPER_CONNECT="${ZOOKEEPER_HOST}:${ZOOKEEPER_PORT}"
BROKER_HOSTS=()
BOOTSTRAP_SERVERS=()
for broker_node in "${BROKER_NODES[@]}"; do
  broker_host="$(node_ip "${broker_node}")"
  BROKER_HOSTS+=("${broker_host}")
  BOOTSTRAP_SERVERS+=("${broker_host}:${BROKER_PORT}")
done
BOOTSTRAP_SERVER="$(IFS=,; echo "${BOOTSTRAP_SERVERS[*]}")"

ZOOKEEPER_CONFIG="${CONFIG_DIR}/kafka-zookeeper.properties"
SERVER_CONFIG="${CONFIG_DIR}/kafka-server.properties"

cat > "${ZOOKEEPER_CONFIG}" <<EOF
dataDir=${ZOOKEEPER_DATA_DIR}
clientPort=${ZOOKEEPER_PORT}
clientPortAddress=0.0.0.0
maxClientCnxns=0
admin.enableServer=false
EOF

cat > "${CONFIG_DIR}/kafka-config-manifest.env" <<EOF
deployment_mode=bare_metal
node_count=${NODE_COUNT}
client_count=${CLIENT_COUNT}
workflow=${WORKFLOW}
message_size_bytes=${MESSAGE_SIZE_BYTES}
operation_count=${OPERATION_COUNT}
partition=${PARTITION}
slurm_time=${SLURM_TIME}
zookeeper_node=${ZOOKEEPER_NODE}
broker_nodes=$(IFS=,; echo "${BROKER_NODES[*]}")
network_iface=${NETWORK_IFACE}
zookeeper_connect=${ZOOKEEPER_CONNECT}
bootstrap_server=${BOOTSTRAP_SERVER}
kafka_home=${KAFKA_HOME}
kafka_acks=${KAFKA_ACKS}
kafka_server_config_pattern=${CONFIG_DIR}/kafka-server-broker-<id>.properties
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
for broker_index in "${!BROKER_NODES[@]}"; do
  echo "Broker ${broker_index} node: ${BROKER_NODES[${broker_index}]} (${BROKER_HOSTS[${broker_index}]})"
done
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

for broker_index in "${!BROKER_NODES[@]}"; do
  broker_node="${BROKER_NODES[${broker_index}]}"
  broker_host="${BROKER_HOSTS[${broker_index}]}"
  broker_config="${CONFIG_DIR}/kafka-server-broker-${broker_index}.properties"
  broker_data_dir="${KAFKA_DATA_DIR}/broker-${broker_index}"
  mkdir -p "${broker_data_dir}"
  cat > "${broker_config}" <<EOF
broker.id=${broker_index}
listeners=PLAINTEXT://0.0.0.0:${BROKER_PORT}
advertised.listeners=PLAINTEXT://${broker_host}:${BROKER_PORT}
log.dirs=${broker_data_dir}
zookeeper.connect=${ZOOKEEPER_CONNECT}
offsets.topic.replication.factor=1
transaction.state.log.replication.factor=1
transaction.state.log.min.isr=1
num.partitions=${#BROKER_NODES[@]}
auto.create.topics.enable=false
delete.topic.enable=true
EOF

  srun --partition="${PARTITION}" --nodes=1 --ntasks=1 --exclusive \
    --time="${SLURM_TIME}" --nodelist="${broker_node}" \
    bash -lc "hostname; export KAFKA_HEAP_OPTS='${KAFKA_HEAP_OPTS_VALUE}'; exec '${KAFKA_HOME}/bin/kafka-server-start.sh' '${broker_config}'" \
    > "${KAFKA_LOG_DIR}/broker-${broker_index}.stdout.log" \
    2> "${KAFKA_LOG_DIR}/broker-${broker_index}.stderr.log" &
  echo "$!" > "${KAFKA_PID_DIR}/broker-${broker_index}.pid"

  if [[ "${broker_index}" == "0" ]]; then
    cp "${broker_config}" "${SERVER_CONFIG}"
  fi

  if ! phase0_wait_for_port "${broker_host}" "${BROKER_PORT}" 60; then
    echo "Kafka broker ${broker_index} failed to listen on ${broker_host}:${BROKER_PORT}" >&2
    exit 1
  fi
done

ADMIN_DEADLINE=$((SECONDS + 60))
until "${KAFKA_HOME}/bin/kafka-topics.sh" --bootstrap-server "${BOOTSTRAP_SERVER}" --list >/dev/null; do
  if (( SECONDS >= ADMIN_DEADLINE )); then
    echo "Kafka broker port opened, but admin client did not become ready for ${BOOTSTRAP_SERVER}" >&2
    exit 1
  fi
  sleep 1
done

"${KAFKA_HOME}/bin/kafka-topics.sh" --bootstrap-server "${BOOTSTRAP_SERVER}" \
  --create --if-not-exists --topic "${TOPIC}" --partitions "${#BROKER_NODES[@]}" --replication-factor 1 \
  > "${KAFKA_RESULT_DIR}/topic-create.log" 2>&1

PRODUCER_START_NS="$(date +%s%N)"
PRODUCER_PIDS=()
for client_index in $(seq 0 $((CLIENT_COUNT - 1))); do
  "${KAFKA_HOME}/bin/kafka-producer-perf-test.sh" \
    --topic "${TOPIC}" \
    --num-records "${OPERATION_COUNT}" \
    --record-size "${MESSAGE_SIZE_BYTES}" \
    --throughput -1 \
    --producer-props "bootstrap.servers=${BOOTSTRAP_SERVER}" "acks=${KAFKA_ACKS}" "client.id=phase0-producer-${client_index}" \
    > "${KAFKA_RESULT_DIR}/producer-perf-append-throughput-client-${client_index}.log" \
    2> "${KAFKA_RESULT_DIR}/producer-perf-append-throughput-client-${client_index}.stderr.log" &
  PRODUCER_PIDS+=("$!")
done
PRODUCER_OK=1
for producer_pid in "${PRODUCER_PIDS[@]}"; do
  if ! wait "${producer_pid}"; then
    PRODUCER_OK=0
  fi
done
PRODUCER_END_NS="$(date +%s%N)"
if [[ "${PRODUCER_OK}" -ne 1 ]]; then
  echo "One or more Kafka producer clients failed" >&2
  exit 1
fi

if [[ "${WORKFLOW}" == "range_retrieval" ]]; then
  TOTAL_OPERATION_COUNT=$((OPERATION_COUNT * CLIENT_COUNT))
  "${KAFKA_HOME}/bin/kafka-consumer-perf-test.sh" \
    --bootstrap-server "${BOOTSTRAP_SERVER}" \
    --topic "${TOPIC}" \
    --messages "${TOTAL_OPERATION_COUNT}" \
    --group "phase0-range-${TOPIC}" \
    --timeout 30000 \
    > "${KAFKA_RESULT_DIR}/consumer-perf-range-retrieval.log" \
    2> "${KAFKA_RESULT_DIR}/consumer-perf-range-retrieval.stderr.log"

  python3 - "${KAFKA_RESULT_DIR}/consumer-perf-range-retrieval.log" "${KAFKA_RESULT_DIR}/metrics.json" "${NODE_COUNT}" "${CLIENT_COUNT}" "${OPERATION_COUNT}" "${MESSAGE_SIZE_BYTES}" "${KAFKA_ACKS}" <<'PY'
import csv
import json
import sys

log_path, metrics_path, node_count, client_count, operation_count, message_size, kafka_acks = sys.argv[1:]
rows = [row for row in csv.reader(open(log_path)) if row]
if len(rows) < 2:
    raise SystemExit(f"Could not parse consumer perf output: {open(log_path).read()}")
header = [col.strip() for col in rows[0]]
values = [col.strip() for col in rows[-1]]
data = dict(zip(header, values))
total_operation_count = int(operation_count) * int(client_count)
records = int(float(data.get("data.consumed.in.nMsg", total_operation_count)))
throughput = float(data.get("nMsg.sec", 0))
duration = records / throughput if throughput else 0
if kafka_acks == "0":
    durability_boundary = "producer_no_broker_ack"
elif kafka_acks in {"all", "-1"}:
    durability_boundary = "leader_and_in_sync_replicas_ack_rf1"
else:
    durability_boundary = "broker_leader_ack_not_all_replicas"
metrics = {
    "system": "kafka",
    "workflow": "range_retrieval",
    "node_count": int(node_count),
    "nodes": int(node_count),
    "client_count": int(client_count),
    "parallel_clients": int(client_count),
    "message_size_bytes": int(message_size),
    "operation_count": int(operation_count),
    "operation_count_per_client": int(operation_count),
    "message_count_per_client": int(operation_count),
    "messages_per_client": int(operation_count),
    "total_operation_count": total_operation_count,
    "total_message_count": total_operation_count,
    "total_messages": total_operation_count,
    "total_payload_bytes": total_operation_count * int(message_size),
    "parallel_client_count": int(client_count),
    "duration_seconds": duration,
    "throughput_ops_per_sec": throughput,
    "avg_latency_ms": None,
    "p50_latency_ms": None,
    "p95_latency_ms": None,
    "p99_latency_ms": None,
    "success": records >= total_operation_count,
    "kafka_acks": kafka_acks,
    "semantic_boundary": f"consumer_catchup_read_after_append_acks_{kafka_acks}",
    "append_ack_boundary": f"kafka_producer_perf_test_acks_{kafka_acks}",
    "durability_boundary": durability_boundary,
    "read_path": "kafka_consumer_perf_from_topic",
    "storage_backend": "kafka_log_dirs",
    "semantic_notes": f"Harness appends first with Kafka acks={kafka_acks}, then measures consumer catch-up read; replication-factor=1.",
}
open(metrics_path, "w").write(json.dumps(metrics, indent=2) + "\n")
print(json.dumps(metrics, indent=2))
if not metrics["success"]:
    raise SystemExit(1)
PY
else
  python3 - "${KAFKA_RESULT_DIR}" "${KAFKA_RESULT_DIR}/metrics.json" "${NODE_COUNT}" "${CLIENT_COUNT}" "${OPERATION_COUNT}" "${MESSAGE_SIZE_BYTES}" "${WORKFLOW}" "${KAFKA_ACKS}" "${PRODUCER_START_NS}" "${PRODUCER_END_NS}" <<'PY'
import json
import re
import sys
from pathlib import Path

result_dir, metrics_path, node_count, client_count, operation_count, message_size, workflow, kafka_acks, start_ns, end_ns = sys.argv[1:]
pattern = re.compile(
    r"(?P<records>\d+) records sent, (?P<tput>[0-9.]+) records/sec .*?, "
    r"(?P<avg>[0-9.]+) ms avg latency, .*?, (?P<p50>[0-9.]+) ms 50th, "
    r"(?P<p95>[0-9.]+) ms 95th, (?P<p99>[0-9.]+) ms 99th"
)
records = 0
avg_weighted = 0.0
p50_values = []
p95_values = []
p99_values = []
client_rows = []
for log_path in sorted(Path(result_dir).glob("producer-perf-append-throughput-client-*.log")):
    if log_path.name.endswith(".stderr.log"):
        continue
    text = log_path.read_text()
    match = pattern.search(text)
    if not match:
        raise SystemExit(f"Could not parse producer perf output {log_path}: {text}")
    client_records = int(match.group("records"))
    records += client_records
    avg_weighted += float(match.group("avg")) * client_records
    p50_values.append(float(match.group("p50")))
    p95_values.append(float(match.group("p95")))
    p99_values.append(float(match.group("p99")))
    client_rows.append({
        "log": str(log_path),
        "records": client_records,
        "throughput_ops_per_sec": float(match.group("tput")),
        "avg_latency_ms": float(match.group("avg")),
        "p50_latency_ms": float(match.group("p50")),
        "p95_latency_ms": float(match.group("p95")),
        "p99_latency_ms": float(match.group("p99")),
    })
wall_duration = (int(end_ns) - int(start_ns)) / 1_000_000_000.0
throughput = records / wall_duration if wall_duration > 0 else 0
total_operation_count = int(operation_count) * int(client_count)
if kafka_acks == "0":
    durability_boundary = "producer_no_broker_ack"
elif kafka_acks in {"all", "-1"}:
    durability_boundary = "leader_and_in_sync_replicas_ack_rf1"
else:
    durability_boundary = "broker_leader_ack_not_all_replicas"
metrics = {
    "system": "kafka",
    "workflow": workflow,
    "node_count": int(node_count),
    "nodes": int(node_count),
    "client_count": int(client_count),
    "parallel_clients": int(client_count),
    "message_size_bytes": int(message_size),
    "operation_count": int(operation_count),
    "operation_count_per_client": int(operation_count),
    "message_count_per_client": int(operation_count),
    "messages_per_client": int(operation_count),
    "total_operation_count": total_operation_count,
    "total_message_count": total_operation_count,
    "total_messages": total_operation_count,
    "total_payload_bytes": total_operation_count * int(message_size),
    "parallel_client_count": int(client_count),
    "duration_seconds": wall_duration,
    "throughput_ops_per_sec": throughput,
    "avg_latency_ms": avg_weighted / records if records else None,
    "p50_latency_ms": max(p50_values) if p50_values else None,
    "p95_latency_ms": max(p95_values) if p95_values else None,
    "p99_latency_ms": max(p99_values) if p99_values else None,
    "success": records == total_operation_count,
    "records_sent": records,
    "client_metrics": client_rows,
    "kafka_acks": kafka_acks,
    "semantic_boundary": f"append_ack_acks_{kafka_acks}",
    "append_ack_boundary": f"kafka_producer_perf_test_acks_{kafka_acks}",
    "durability_boundary": durability_boundary,
    "read_path": "",
    "storage_backend": "kafka_log_dirs",
    "semantic_notes": f"Kafka producer perf uses acks={kafka_acks} and replication-factor=1 in this harness.",
}
open(metrics_path, "w").write(json.dumps(metrics, indent=2) + "\n")
print(json.dumps(metrics, indent=2))
if not metrics["success"]:
    raise SystemExit(1)
PY
fi

cat > "${RESULT_DIR}/summary.md" <<EOF
# Kafka Distributed Append Smoke

- system: Kafka
- workflow: ${WORKFLOW}
- deployment_mode: bare_metal
- node_count: ${NODE_COUNT}
- nodes: ${NODE_COUNT}
- client_count: ${CLIENT_COUNT}
- parallel_clients: ${CLIENT_COUNT}
- parallel_client_count: ${CLIENT_COUNT}
- zookeeper_node: ${ZOOKEEPER_NODE}
- broker_nodes: $(IFS=,; echo "${BROKER_NODES[*]}")
- bootstrap_server: ${BOOTSTRAP_SERVER}
- kafka_acks: ${KAFKA_ACKS}
- operation_count: ${OPERATION_COUNT}
- operation_count_per_client: ${OPERATION_COUNT}
- message_count_per_client: ${OPERATION_COUNT}
- messages_per_client: ${OPERATION_COUNT}
- total_operation_count: $((OPERATION_COUNT * CLIENT_COUNT))
- total_message_count: $((OPERATION_COUNT * CLIENT_COUNT))
- total_messages: $((OPERATION_COUNT * CLIENT_COUNT))
- message_size_bytes: ${MESSAGE_SIZE_BYTES}
- total_payload_bytes: $((OPERATION_COUNT * CLIENT_COUNT * MESSAGE_SIZE_BYTES))
- metrics: kafka/metrics.json
EOF
