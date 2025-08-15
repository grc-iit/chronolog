#!/bin/bash

REP=3
EVENT_SIZE_MIN=4096
EVENT_SIZE_MAX=4096
EVENT_SIZE_AVE=4096
EVENT_COUNT=1000
STORY_COUNT=1
CHRONICLE_COUNT=1
SHARED_STORY=false
BARRIER=true

NUM_NODES=4
NUM_PROCS=4
BUILD_TYPE=Release
CHRONOLOG_INSTALL_DIR=/home/${USER}/chronolog/${BUILD_TYPE}
CHRONOLOG_BIN_DIR=${CHRONOLOG_INSTALL_DIR}/bin
CHRONOLOG_LIB_DIR=${CHRONOLOG_INSTALL_DIR}/lib
HOST_FILE=${CHRONOLOG_INSTALL_DIR}/conf/hosts_client
CLIENT_ADMIN_BIN=${CHRONOLOG_BIN_DIR}/client_admin
MPIEXEC_BIN="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../.spack-env/view/bin/mpiexec"
CONF_FILE=/home/${USER}/chronolog/${BUILD_TYPE}/conf/default_conf.json
OUTPUT_LOG_FILE=./perf_test.log

[[ -f ${HOST_FILE} ]] || { echo "Host file not found: ${HOST_FILE}"; exit 1; }
[[ -f ${CLIENT_ADMIN_BIN} ]] || { echo "client_admin binary not found: ${CLIENT_ADMIN_BIN}"; exit 1; }
[[ -f ${MPIEXEC_BIN} ]] || { echo "mpiexec binary not found: ${MPIEXEC_BIN}"; exit 1; }
[[ -f ${CONF_FILE} ]] || { echo "Configuration file not found: ${CONF_FILE}"; exit 1; }

rm -f ${OUTPUT_LOG_FILE}
rm -f ${HOST_FILE}.*

head -${NUM_NODES} ${HOST_FILE} > "${HOST_FILE}.${NUM_NODES}"
cli_args="-a ${EVENT_SIZE_MIN} -b ${EVENT_SIZE_MAX} -s ${EVENT_SIZE_AVE} -n ${EVENT_COUNT} -t ${STORY_COUNT} -h ${CHRONICLE_COUNT} -p"
[[ "S{SHARED_STORY}" == "true" ]] && cli_args+=" -o"
[[ "${BARRIER}" == "true" ]] && cli_args+=" -r"
for i in $(seq 1 ${REP})
do
    echo "======================================================================================" >> ${OUTPUT_LOG_FILE}
    echo "Iteration ${i}" >> ${OUTPUT_LOG_FILE}
    echo LD_LIBRARY_PATH=${CHRONOLOG_LIB_DIR} ${MPIEXEC_BIN} -n ${NUM_PROCS} -f "${HOST_FILE}.${NUM_NODES}" \
        "${CLIENT_ADMIN_BIN}" -c "${CONF_FILE}" ${cli_args}
    LD_LIBRARY_PATH=${CHRONOLOG_LIB_DIR} ${MPIEXEC_BIN} -n ${NUM_PROCS} -f "${HOST_FILE}.${NUM_NODES}" \
        "${CLIENT_ADMIN_BIN}" -c "${CONF_FILE}" ${cli_args} >>${OUTPUT_LOG_FILE} 2>&1
done

