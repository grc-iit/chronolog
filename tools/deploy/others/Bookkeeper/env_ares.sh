#!/bin/bash

# Zookeeper
KAFKA_ROOT_DIR=/mnt/common/kfeng/pkg_src/kafka_2.13-2.8.0
source ${KAFKA_ROOT_DIR}/scripts/env.sh

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
BOOKKEEPER_ROOT_DIR=/mnt/common/kfeng/pkg_src/bookkeeper

HOSTNAME=`head -1 ${CWD}/servers`
HOSTNAME_POSTFIX=-40g
if [[ ${HOSTNAME} == *comp* ]]
then
  drive="nvme"
elif [[ ${HOSTNAME} == *stor* ]]
then
  drive="hdd"
fi

# Bookkeeper
BOOKKEEPER_JOURNAL_DIR=/mnt/${drive}/kfeng/bk-txn
BOOKKEEPER_LEDGER_DIR=/mnt/${drive}/kfeng/bk-data
BOOKKEEPER_BOOKIE_PID_DIR=/dev/shm
BOOKKEEPER_SERVERS_HOSTFILE=${CWD}/servers
BOOKKEEPER_BOOKIES_FILE=${BOOKKEEPER_ROOT_DIR}/conf/bookies
BOOKKEEPER_N_SERVERS=`wc -l ${BOOKKEEPER_SERVERS_HOSTFILE} | cut -d' ' -f1`
BOOKKEEPER_SERVERS=`cat ${BOOKKEEPER_SERVERS_HOSTFILE} | awk '{print $1}'`
BOOKKEEPER_CONF_FILE=${BOOKKEEPER_ROOT_DIR}/conf/bk_server.conf
BOOKKEEPER_ENV_FILE=${BOOKKEEPER_ROOT_DIR}/conf/bkenv.sh
BOOKKEEPER_BOOKIE_PORT=3181
