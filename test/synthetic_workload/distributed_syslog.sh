#!/bin/bash
# Distributed syslog ingest via chrono-client-admin. Run manually.
# Uses INSTALL_DIR (default $HOME/chronolog-install) so that INSTALL_DIR/chronolog is the work tree (bin, conf).

NUM_NODES=4
NUM_PROCS=96
INPUT_FILE=/var/log/syslog

# Standard install layout (matches perf_test.sh and tools/deploy/ChronoLog/install.sh)
INSTALL_DIR="${INSTALL_DIR:-$HOME/chronolog-install}"
CHRONOLOG_WORK_DIR="${INSTALL_DIR}/chronolog"
CHRONOLOG_BIN_DIR="${CHRONOLOG_WORK_DIR}/bin"
HOST_FILE="${CHRONOLOG_WORK_DIR}/conf/hosts_client"
CLIENT_ADMIN_BIN="${CHRONOLOG_BIN_DIR}/chrono-client-admin"
CONF_FILE="${CHRONOLOG_WORK_DIR}/conf/default-chrono-conf.json"

[[ -f ${HOST_FILE} ]] || { echo "Host file not found: ${HOST_FILE}"; exit 1; }
[[ -f ${CLIENT_ADMIN_BIN} ]] || { echo "chrono-client-admin not found: ${CLIENT_ADMIN_BIN}"; exit 1; }
[[ -f ${CONF_FILE} ]] || { echo "Configuration not found: ${CONF_FILE}"; exit 1; }

head -${NUM_NODES} ${HOST_FILE} > "${HOST_FILE}.${NUM_NODES}"
mpiexec -n ${NUM_PROCS} -f "${HOST_FILE}.${NUM_NODES}" "${CLIENT_ADMIN_BIN}" -c "${CONF_FILE}" -f "${INPUT_FILE}"