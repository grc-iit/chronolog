#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

mpssh -f ${BOOKKEEPER_SERVERS_HOSTFILE} "mkdir -p ${BOOKKEEPER_JOURNAL_DIR}"
mpssh -f ${BOOKKEEPER_SERVERS_HOSTFILE} "mkdir -p ${BOOKKEEPER_LEDGER_DIR}"
first_node=`head -1 ${BOOKKEEPER_SERVERS_HOSTFILE}`
if [[ $1 == "-f" ]]
then
  echo "Formatting ..."
  mpssh -f ${BOOKKEEPER_SERVERS_HOSTFILE} "rm -rf ${BOOKKEEPER_JOURNAL_DIR}/*; rm -rf ${BOOKKEEPER_LEDGER_DIR}/*"
  ssh ${first_node} "export JAVA_HOME=${JAVA_HOME}; ${BOOKKEEPER_ROOT_DIR}/bin/bookkeeper shell metaformat -f -n"
else
  echo "Starting Bookkeeper without formatting ..."
fi
ssh ${first_node} "export JAVA_HOME=${JAVA_HOME}; ${BOOKKEEPER_ROOT_DIR}/bin/bookkeeper-cluster.sh start"
mpssh -f ${BOOKKEEPER_SERVERS_HOSTFILE} "jps" | sort -u
ssh ${first_node} "export JAVA_HOME=${JAVA_HOME}; ${BOOKKEEPER_ROOT_DIR}/bin/bookkeeper shell bookiesanity" | grep "Bookie sanity test succeeded"
