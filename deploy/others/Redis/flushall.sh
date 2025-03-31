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

echo -e "${GREEN}Flushing all data ...${NC}"
servers=`cat ${CWD}/servers | awk '{print $1}'`
nservers=`cat ${CWD}/servers | wc -l`
count=0
for server in ${servers[@]}
do
  ((port=$PORT_BASE+$count))
  echo flushall | ${REDIS_DIR}/src/redis-cli -c -h ${server} -p ${port} &
  ((count=$count+1))
done
wait
