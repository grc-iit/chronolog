#!/bin/bash

REP=3
EVENT_SIZE_MIN=16
EVENT_SIZE_MAX=1024
EVENT_SIZE_AVE=512
EVENT_COUNT=100
STORY_COUNT=1
CHRONICLE_COUNT=1
SHARED_STORY=false
BARRIER=true

NUM_NODES=1
NUM_PROCS=2
BUILD_TYPE=Debug
CHRONOLOG_INSTALL_DIR=/home/${USER}/chronolog/${BUILD_TYPE}
CHRONOLOG_BIN_DIR=${CHRONOLOG_INSTALL_DIR}/bin
CHRONOLOG_LIB_DIR=${CHRONOLOG_INSTALL_DIR}/lib
HOST_FILE=${CHRONOLOG_INSTALL_DIR}/conf/hosts_client
CLIENT_ADMIN_BIN=${CHRONOLOG_BIN_DIR}/client_admin
MPIEXEC_BIN=/home/${USER}/spack/opt/spack/linux-ubuntu22.04-zen3/gcc-11.4.0/mpich-4.0.2-nb7jqr5onqb7g7ynodbtsnqrax5sofyo/bin/mpiexec
CONF_FILE=/home/${USER}/chronolog/${BUILD_TYPE}/conf/default_conf.json
OUTPUT_LOG_FILE=./perf_test.log

rm -f ${OUTPUT_LOG_FILE}
head -${NUM_NODES} ${HOST_FILE} > "${HOST_FILE}.${NUM_NODES}"
cli_args_args="-a ${EVENT_SIZE_MIN} -b ${EVENT_SIZE_MAX} -s ${EVENT_SIZE_AVE} -n ${EVENT_COUNT} -t ${STORY_COUNT} -h ${CHRONICLE_COUNT} -p"
[[ "S{SHARED_STORY}" == "true" ]] && cli_args_args+=" -o"
[[ "${BARRIER}" == "true" ]] && cli_args_args+=" -r"
for i in $(seq 1 ${REP}); do
    echo "======================================================================================" >> ${OUTPUT_LOG_FILE}
    echo "Iteration ${i}" >> ${OUTPUT_LOG_FILE}
    echo LD_LIBRARY_PATH=${CHRONOLOG_LIB_DIR} ${MPIEXEC_BIN} -n ${NUM_PROCS} -f "${HOST_FILE}.${NUM_NODES}" \
             "${CLIENT_ADMIN_BIN}" -c "${CONF_FILE}" "${cli_args_args}"
    LD_LIBRARY_PATH=${CHRONOLOG_LIB_DIR} ${MPIEXEC_BIN} -n ${NUM_PROCS} -f "${HOST_FILE}.${NUM_NODES}" \
    "${CLIENT_ADMIN_BIN}" -c "${CONF_FILE}" "${cli_args_args}" >> ${OUTPUT_LOG_FILE} 2>&1
done