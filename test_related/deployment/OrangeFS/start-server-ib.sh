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

PVFS2_BIN="$PVFS2_HOME/sbin/pvfs2-server"
PVFS2_PING="$PVFS2_HOME/bin/pvfs2-ping"

echo -e "${GREEN}Starting OrangeFS servers ...${NC}"
mpssh -f $CWD/servers "$PVFS2_BIN $CWD/pvfs2-${number}N-ib.conf -f"
mpssh -f $CWD/servers "$PVFS2_BIN $CWD/pvfs2-${number}N-ib.conf"
mpssh -f $CWD/servers "pgrep -al pvfs2-server"

sleep 5
echo -e "${GREEN}Verifying OrangeFS servers ...${NC}"
mpssh -f $CWD/clients "export LD_LIBRARY_PATH=$PVFS2_HOME/lib; export PVFS2TAB_FILE=$PVFS2TAB_FILE_CLIENT; $PVFS2_PING -m $MOUNT_POINT | grep 'appears to be correctly configured'" | sort
