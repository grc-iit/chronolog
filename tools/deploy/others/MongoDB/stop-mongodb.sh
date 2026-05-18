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

source ~/.bash_aliases

echo -e "${GREEN}Stopping MongoDB processes ...${NC}"
mpssh -f ${CWD}/servers 'pkill mongod' > /dev/null
mpssh -f ${CWD}/clients 'pkill mongos' > /dev/null
mpssh -f ${CWD}/clients 'rm -f /tmp/mongodb-*.sock' > /dev/null

[[ "${BASH_SOURCE[0]}" == "${0}" ]] && echo -e "${GREEN}Done stopping MongoDB${NC}"
