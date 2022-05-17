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
  exit 1
fi

${CWD}/stop.sh

echo -e "${GREEN}Cleaning Redis ...${NC}"
i=0
for server in ${SERVERS[@]}
do
  ((port=$PORT_BASE+$i))
  #ssh $server "rm -rf $LOCAL_DIR/$port/*.aof $LOCAL_DIR/$port/*.rdb $LOCAL_DIR/$port/nodes.conf $LOCAL_DIR/$port/file.log"
  ssh $server "rm -rf $LOCAL_DIR/*"
  ((i=i+1))
done
echo -e "${GREEN}Previous Redis is cleaned${NC}"
