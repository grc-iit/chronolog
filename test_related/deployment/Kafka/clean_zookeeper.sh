#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

source ${CWD}/stop_zookeeper.sh

mpssh -f ${ZOOKEEPER_SERVERS_HOSTFILE} "rm -rf ${ZOOKEEPER_DATA_DIR}/*"
