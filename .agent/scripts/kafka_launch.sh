#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.agent/scripts/kafka_common.sh
source "${SCRIPT_DIR}/kafka_common.sh"

usage() {
  cat <<'USAGE'
Usage: kafka_launch.sh [options]

Launch a Phase 0 single-node Kafka fixed baseline and write all runtime
artifacts under .agent/results.

Options:
  --result-dir DIR        Existing or new result directory to use.
  --host HOST             Listener host. Default: 127.0.0.1.
  --zookeeper-port PORT   ZooKeeper client port. Default: 22181.
  --broker-port PORT      Kafka broker port. Default: 29092.
  --wait-seconds N        Startup wait timeout per service. Default: 30.
  -h, --help              Show this help.
USAGE
}

RESULT_DIR="${PHASE0_RESULT_DIR:-}"
HOST="${KAFKA_HOST:-127.0.0.1}"
ZOOKEEPER_PORT="${KAFKA_ZOOKEEPER_PORT:-22181}"
BROKER_PORT="${KAFKA_BROKER_PORT:-29092}"
WAIT_SECONDS="${KAFKA_WAIT_SECONDS:-30}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --result-dir)
      RESULT_DIR="$2"
      shift 2
      ;;
    --host)
      HOST="$2"
      shift 2
      ;;
    --zookeeper-port)
      ZOOKEEPER_PORT="$2"
      shift 2
      ;;
    --broker-port)
      BROKER_PORT="$2"
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

RESULT_DIR="$(phase0_make_result_dir "${RESULT_DIR}")"
CONFIG_DIR="${RESULT_DIR}/config"
KAFKA_RESULT_DIR="${RESULT_DIR}/kafka"
KAFKA_LOG_DIR="${KAFKA_RESULT_DIR}/logs"
KAFKA_DATA_DIR="${KAFKA_RESULT_DIR}/data"
KAFKA_PID_DIR="${KAFKA_RESULT_DIR}/pids"
ZOOKEEPER_DATA_DIR="${KAFKA_RESULT_DIR}/zookeeper"

mkdir -p "${CONFIG_DIR}" "${KAFKA_LOG_DIR}" "${KAFKA_DATA_DIR}" "${KAFKA_PID_DIR}" "${ZOOKEEPER_DATA_DIR}"

exec > >(tee -a "${KAFKA_RESULT_DIR}/stdout.log")
exec 2> >(tee -a "${KAFKA_RESULT_DIR}/stderr.log" >&2)

KAFKA_HOME="$(phase0_install_kafka_if_needed)"
BOOTSTRAP_SERVER="${HOST}:${BROKER_PORT}"
ZOOKEEPER_CONNECT="${HOST}:${ZOOKEEPER_PORT}"

ZOOKEEPER_CONFIG="${CONFIG_DIR}/kafka-zookeeper.properties"
SERVER_CONFIG="${CONFIG_DIR}/kafka-server.properties"
LAUNCH_ENV="${CONFIG_DIR}/kafka-launch.env"

cat > "${ZOOKEEPER_CONFIG}" <<EOF
dataDir=${ZOOKEEPER_DATA_DIR}
clientPort=${ZOOKEEPER_PORT}
maxClientCnxns=0
admin.enableServer=false
EOF

cat > "${SERVER_CONFIG}" <<EOF
broker.id=0
listeners=PLAINTEXT://${HOST}:${BROKER_PORT}
advertised.listeners=PLAINTEXT://${HOST}:${BROKER_PORT}
log.dirs=${KAFKA_DATA_DIR}
zookeeper.connect=${ZOOKEEPER_CONNECT}
offsets.topic.replication.factor=1
transaction.state.log.replication.factor=1
transaction.state.log.min.isr=1
num.partitions=1
auto.create.topics.enable=false
delete.topic.enable=true
EOF

cat > "${LAUNCH_ENV}" <<EOF
KAFKA_HOME=${KAFKA_HOME}
KAFKA_BOOTSTRAP_SERVER=${BOOTSTRAP_SERVER}
KAFKA_ZOOKEEPER_CONNECT=${ZOOKEEPER_CONNECT}
KAFKA_RESULT_DIR=${KAFKA_RESULT_DIR}
KAFKA_SERVER_CONFIG=${SERVER_CONFIG}
KAFKA_ZOOKEEPER_CONFIG=${ZOOKEEPER_CONFIG}
EOF

echo "Kafka home: ${KAFKA_HOME}"
echo "Result directory: ${RESULT_DIR}"
echo "Bootstrap server: ${BOOTSTRAP_SERVER}"
echo "ZooKeeper connect: ${ZOOKEEPER_CONNECT}"

"${KAFKA_HOME}/bin/zookeeper-server-start.sh" "${ZOOKEEPER_CONFIG}" \
  > "${KAFKA_LOG_DIR}/zookeeper.stdout.log" \
  2> "${KAFKA_LOG_DIR}/zookeeper.stderr.log" &
ZOOKEEPER_PID=$!
echo "${ZOOKEEPER_PID}" > "${KAFKA_PID_DIR}/zookeeper.pid"

if ! phase0_wait_for_port "${HOST}" "${ZOOKEEPER_PORT}" "${WAIT_SECONDS}"; then
  echo "ZooKeeper failed to listen on ${ZOOKEEPER_CONNECT}" >&2
  exit 1
fi

"${KAFKA_HOME}/bin/kafka-server-start.sh" "${SERVER_CONFIG}" \
  > "${KAFKA_LOG_DIR}/broker.stdout.log" \
  2> "${KAFKA_LOG_DIR}/broker.stderr.log" &
BROKER_PID=$!
echo "${BROKER_PID}" > "${KAFKA_PID_DIR}/broker.pid"

if ! phase0_wait_for_port "${HOST}" "${BROKER_PORT}" "${WAIT_SECONDS}"; then
  echo "Kafka broker failed to listen on ${BOOTSTRAP_SERVER}" >&2
  exit 1
fi

"${KAFKA_HOME}/bin/kafka-topics.sh" --bootstrap-server "${BOOTSTRAP_SERVER}" --list >/dev/null

echo "Kafka fixed baseline is running"
