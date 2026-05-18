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

source ${CWD}/stop-client.sh
source ${CWD}/stop-server.sh

echo -e "${GREEN}Cleaning OrangeFS ...${NC}"
mpssh -f $CWD/servers "rm -rf ${SERVER_LOG_FILE}"
mpssh -f $CWD/servers "rm -rf ${SERVER_LOCAL_STOR_DIR}/*"
mpssh -f $CWD/clients "truncate -s 0 ${PVFS2TAB_FILE_CLIENT}"
