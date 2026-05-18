#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

java_home_esc=$(sed 's/\//\\&/g' <<< "${JAVA_HOME}")
sed -i "s/^\#\s*JAVA_HOME=.*\|^JAVA_HOME=.*/JAVA_HOME=${java_home_esc}/" ${BOOKKEEPER_ENV_FILE}
bookie_pid_dir_esc=$(sed 's/\//\\&/g' <<<"${BOOKKEEPER_BOOKIE_PID_DIR}")
sed -i "s/^\#\s*BOOKIE_PID_DIR=.*\|^BOOKIE_PID_DIR=.*/BOOKIE_PID_DIR=${bookie_pid_dir_esc}/" ${BOOKKEEPER_ENV_FILE}
bookkeeper_journal_dir_esc=$(sed 's/\//\\&/g' <<<"${BOOKKEEPER_JOURNAL_DIR}")
sed -i "s/^journalDirectories=.*/journalDirectories=${bookkeeper_journal_dir_esc}/" ${BOOKKEEPER_CONF_FILE}
bookkeeper_ledger_dir_esc=$(sed 's/\//\\&/g' <<<"${BOOKKEEPER_LEDGER_DIR}")
sed -i "s/^ledgerDirectories=.*/ledgerDirectories=${bookkeeper_ledger_dir_esc}/" ${BOOKKEEPER_CONF_FILE}
sed -i "s/^bookiePort=.*/bookiePort=${BOOKKEEPER_BOOKIE_PORT}/" ${BOOKKEEPER_CONF_FILE}
sed -i "s/^zkServers=.*/zkServers=${kafka_zookeeper_connect}/" ${BOOKKEEPER_CONF_FILE}
cp ${BOOKKEEPER_SERVERS_HOSTFILE} ${BOOKKEEPER_BOOKIES_FILE}
