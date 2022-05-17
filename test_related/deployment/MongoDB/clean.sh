#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

source ${CWD}/stop-mongodb.sh

echo -e "${GREEN}Removing MongoDB directories ...${NC}"
mpssh -f ${CWD}/servers "rm -rf ${mongod_config_path} ${mongod_shard_path}" > /dev/null
mpssh -f ${CWD}/clients "rm -rf ${mongos_local_path}" > /dev/null

echo -e "${GREEN}Removing MongoDB conf files ...${NC}"
mpssh -f ${CWD}/servers "rm -rf ${SERVER_LOCAL_PATH}/${MONGOD_CONFIG_CONF_FILE} ${SERVER_LOCAL_PATH}/${MONGOD_SHARD_CONF_FILE}" > /dev/null
mpssh -f ${CWD}/clients "rm -rf ${CLIENT_LOCAL_PATH}/${MONGOS_CONF_FILE}" > /dev/null

echo -e "${GREEN}Removing MongoDB log files ...${NC}"
mpssh -f ${CWD}/servers "rm -rf ${TMPFS_PATH}/${MONGOD_CONFIG_LOG_FILE} ${TMPFS_PATH}/${MONGOD_SHARD_LOG_FILE}" > /dev/null
mpssh -f ${CWD}/clients "rm -rf ${TMPFS_PATH}/${MONGOS_LOG_FILE} ${mongos_diag_data_path}" > /dev/null

echo -e "${GREEN}Checking remaining processes ...${NC}"
server_pgrep=`mpssh -f ${CWD}/servers "pgrep -la \"mongod|mongos\""`
client_pgrep=`mpssh -f ${CWD}/clients "pgrep -la \"mongod|mongos\""`
if [[ ! -z ${server_pgrep} ]] || [[ ! -z ${client_pgrep} ]]
then
  echo -e "${RED}Something is left there${NC}"
  mpssh -f ${CWD}/servers "pgrep -la \"mongod|mongos\""
  mpssh -f ${CWD}/clients "pgrep -la \"mongod|mongos\""
else
  echo -e "${GREEN}Everything is cleaned${NC}"
fi

echo -e "${GREEN}Done cleaning MongoDB${NC}"
