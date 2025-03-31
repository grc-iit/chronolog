#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

PVFS2_GENCONFIG="$PVFS2_HOME/bin/pvfs2-genconfig"

$PVFS2_GENCONFIG --quiet --protocol ib --ioservers $servers --metaservers $servers --storage $SERVER_LOCAL_PATH/pvfs2-storage-space --metadata $SERVER_LOCAL_PATH/pvfs2-storage-space --logfile $TMPFS_PATH/orangefs-server.log $CWD/pvfs2-${number}N-ib.conf

if [ "$1" == "sync" ]
then
  sed -i "s/TroveSyncData.*/TroveSyncData yes/" $CWD/pvfs2-${number}N-ib.conf
fi

client_list=`cat ${CWD}/clients | awk '{print $1}'`
count=1
for client in ${client_list[@]}
do
  metadata_server=`head -$count ${CWD}/servers | tail -1`
  metadata_server_ip=`getent hosts ${metadata_server}${hs_hostname_suffix} | awk '{print $1}'`
  ssh $client "echo 'ib://$metadata_server_ip:3335/orangefs $MOUNT_POINT pvfs2 defaults,auto 0 0' > $PVFS2TAB_FILE_CLIENT" &
  ((count=$count+1))
done
wait

# sync files only on Chameleon
if [ "$USER" == "cc" ]
then
  server_list=`cat ${CWD}/servers | awk '{print $1}'`
  for node in ${server_list[@]}
  do
    rsync -az $CWD/pvfs2-${number}N-ib.conf $node:$CWD/pvfs2-${number}N-ib.conf
    rsync -az $PVFS2TAB_FILE $node:$PVFS2TAB_FILE
  done
fi

# replace hostname with high-speed one on Ares
if [ "$USER" == "kfeng" ]
then
  sed -i 's/:\/\/ares-stor-0/:\/\/172.25.201./' $CWD/pvfs2-${number}N-ib.conf
  sed -i 's/:\/\/ares-stor-/:\/\/172.25.201./' $CWD/pvfs2-${number}N-ib.conf
  sed -i 's/:\/\/ares-comp-/:\/\/172.25.101./' $CWD/pvfs2-${number}N-ib.conf
fi
