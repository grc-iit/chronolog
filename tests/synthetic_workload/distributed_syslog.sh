#!/bin/bash
# Distributed syslog ingest via chrono-bench. Run manually.
# Uses INSTALL_DIR (default $HOME/chronolog-install) so that INSTALL_DIR/chronolog is the work tree (bin, conf).

NUM_NODES=4
NUM_PROCS=96
INPUT_FILE=/var/log/syslog

# Standard install layout
INSTALL_DIR="${INSTALL_DIR:-$HOME/chronolog-install}"
CHRONOLOG_WORK_DIR="${INSTALL_DIR}/chronolog"
CHRONOLOG_BENCHMARK_DIR="${CHRONOLOG_WORK_DIR}/tools/benchmark"
HOST_FILE="${CHRONOLOG_WORK_DIR}/conf/hosts_client"
BENCH_BIN="${CHRONOLOG_BENCHMARK_DIR}/chrono-bench"
CONF_FILE="${CHRONOLOG_WORK_DIR}/conf/default-chrono-conf.json"

[[ -f ${HOST_FILE} ]] || { echo "Host file not found: ${HOST_FILE}"; exit 1; }
[[ -f ${BENCH_BIN} ]] || { echo "chrono-bench not found: ${BENCH_BIN}"; exit 1; }
[[ -f ${CONF_FILE} ]] || { echo "Configuration not found: ${CONF_FILE}"; exit 1; }

head -${NUM_NODES} ${HOST_FILE} > "${HOST_FILE}.${NUM_NODES}"
mpiexec -n ${NUM_PROCS} -f "${HOST_FILE}.${NUM_NODES}" "${BENCH_BIN}" -c "${CONF_FILE}" -f "${INPUT_FILE}"