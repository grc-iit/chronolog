#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

source ${CWD}/stop_bookkeeper.sh

mpssh -f ${BOOKKEEPER_SERVERS_HOSTFILE} "rm -rf ${BOOKKEEPER_JOURNAL_DIR}/*; rm -rf ${BOOKKEEPER_LEDGER_DIR}/*"
rm -rf ${BOOKKEEPER_ROOT_DIR}/logs/*

