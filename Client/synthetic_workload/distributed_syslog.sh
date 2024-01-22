#!/bin/bash

NUM_NODES=4
NUM_PROCS=96
HOST_FILE=client_hosts
CLIENT_ADMIN_BIN=/home/${USER}/chronolog/bin/client_admin
CONF_FILE=/home/${USER}/chronolog/conf/default_conf.json
INPUT_FILE=/var/log/syslog

head -${NUM_NODES} ${HOST_FILE} > "${HOST_FILE}.${NUM_NODES}"
mpiexec -n ${NUM_PROCS} -f "${HOST_FILE}.${NUM_NODES}" "${CLIENT_ADMIN_BIN}" -c "${CONF_FILE}" -f "${INPUT_FILE}"