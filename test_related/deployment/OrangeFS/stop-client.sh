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

clients=`awk '{printf("%s,",$1)}' ${CWD}/clients`

echo -e "${GREEN}Stopping OrangeFS clients ...${NC}"
if [[ ! -z ${PVFS2_SRC_HOME} ]]
then
  #unmount pvfs2
  mpssh -f ${CWD}/clients "sudo umount -l $MOUNT_POINT"
  mpssh -f ${CWD}/clients "sudo umount -f $MOUNT_POINT"
  mpssh -f ${CWD}/clients "sudo umount $MOUNT_POINT"
  #Kill client process
  mpssh -f ${CWD}/clients "sudo killall -9 pvfs2-client"
  mpssh -f ${CWD}/clients "sudo killall -9 pvfs2-client-core"
  # remove pvfs2 from kernel
  mpssh -f ${CWD}/clients "sudo rmmod pvfs2"
else
  mpssh -f ${CWD}/clients "sudo kill-pvfs2-client"
fi

echo -e "${GREEN}Double checking mount point ...${NC}"
mpssh -f ${CWD}/clients "mount | grep pvfs2" | sort

echo -e "${GREEN}Double checking kernel module ...${NC}"
mpssh -f ${CWD}/clients "/usr/sbin/lsmod | grep pvfs" | sort

echo -e "${GREEN}Double checking client process ...${NC}"
mpssh -f ${CWD}/clients "pgrep -la pvfs2-client" | sort
