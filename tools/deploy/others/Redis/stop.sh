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

echo -e "${GREEN}Stopping Redis ...${NC}"
mpssh -f ${CWD}/servers 'killall redis-server' > /dev/null

echo -e "${GREEN}Double checking redis-server process ...${NC}"
mpssh -f ${CWD}/servers "pgrep -la redis-server" | sort

echo -e "${GREEN}Please check remaining redis-server processes!!!${NC}"
