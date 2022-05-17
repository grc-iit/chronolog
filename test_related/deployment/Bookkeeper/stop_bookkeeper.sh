#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

first_node=`head -1 ${BOOKKEEPER_SERVERS_HOSTFILE}`
ssh ${first_node} "export JAVA_HOME=${JAVA_HOME}; ${BOOKKEEPER_ROOT_DIR}/bin/bookkeeper-cluster.sh stop"
mpssh -f ${BOOKKEEPER_SERVERS_HOSTFILE} "jps -l | grep org.apache.bookkeeper | cut -d' ' -f1 | xargs -r kill -9"
