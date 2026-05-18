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

echo -e "${GREEN}Stopping OrangeFS servers ...${NC}"
mpssh -f ${CWD}/servers "killall -9 pvfs2-server"
mpssh -f ${CWD}/servers "pgrep -al pvfs2-server"

echo -e "${GREEN}Double checking server process ...${NC}"
mpssh -f ${CWD}/clients "pgrep -la pvfs2-server" | sort
