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

if [ "$#" -eq 1 ]
then
  if [ "$1" == "ib" ]
  then
    proto="ib"
    ((port=${comm_port}+1))
  elif [ "$1" == "tcp" ]
  then
    proto="tcp"
    port=${comm_port}
  else
    echo "unrecognized protocol: $1, supported protocols: tcp and ib, quiting ..."
    exit
  fi
else
  proto="tcp"
  port=${comm_port}
fi

source ~/.bash_aliases

echo -e "${GREEN}Starting OrangeFS clients ...${NC}"
if [[ ! -z ${PVFS2_SRC_HOME} ]]
then
  KERNEL_DIR="${PVFS2_SRC_HOME}/src/kernel/linux-2.6"
  CLIENT_DIR="${PVFS2_SRC_HOME}/src/apps/kernel/linux"
else
  KERNEL_DIR="${PVFS2_HOME}/lib/modules/`uname -r`/kernel/fs/pvfs2"
  CLIENT_DIR="${PVFS2_HOME}/sbin"
fi

clients=`awk '{print $1}' ${CWD}/clients`

#insert pvfs2 module into kernel
mpssh -f ${CWD}/clients "sudo insmod ${KERNEL_DIR}/pvfs2.ko"

#start pvfs2 client
mpssh -f ${CWD}/clients "sudo ${CLIENT_DIR}/pvfs2-client -p ${CLIENT_DIR}/pvfs2-client-core"

#mount pvfs2
nservers=`cat ${CWD}/servers | wc -l`
i=1
for node in ${clients[@]}
do
  meta_server=`head -${i} ${CWD}/servers | tail -1`
  ssh ${node} "sudo mount -t pvfs2 ${proto}://${meta_server}${hs_hostname_suffix}:${port}/orangefs ${MOUNT_POINT}"
  ((i=$i+1))
  if [ "${i}" -gt "${nservers}" ]
  then
    i=1
  fi
done

#check mounted pvfs2
mpssh -f ${CWD}/clients "mount | grep pvfs" | sort > ${CWD}/tmp
nentries=`cat ${CWD}/tmp | wc -l`
nclients=`cat ${CWD}/clients | wc -l`
if [[ ${nentries} != ${nclients} ]]
then
  echo -e "${RED}Something is wrong!${NC}"
  echo -e "${RED}Mount entries count: ${nentries}${NC}"
  echo -e "${RED}Client nodes count: ${nclients}${NC}"
else
  cat ${CWD}/tmp
  rm -f ${CWD}/tmp
fi
