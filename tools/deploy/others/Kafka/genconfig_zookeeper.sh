#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

# Zookeeper
mpssh -f ${ZOOKEEPER_SERVERS_HOSTFILE} "mkdir -p ${ZOOKEEPER_DATA_DIR}"
cp ${KAFKA_ROOT_DIR}/config/zookeeper.properties ${ZOOKEEPER_CONF_FILE}
zookeeper_data_dir_esc=$(sed 's/\//\\&/g' <<<"${ZOOKEEPER_DATA_DIR}")
sed -i "s/^dataDir=.*/dataDir=${zookeeper_data_dir_esc}/" ${ZOOKEEPER_CONF_FILE}
sed -i "s/^clientPort=.*/clientPort=${ZOOKEEPER_CLIENT_PORT}/" ${ZOOKEEPER_CONF_FILE}

echo "tickTime=2000" >> ${ZOOKEEPER_CONF_FILE}
echo "initLimit=10" >> ${ZOOKEEPER_CONF_FILE}
echo "syncLimit=5" >> ${ZOOKEEPER_CONF_FILE}

id=1
for zookeeper_server in ${ZOOKEEPER_SERVERS[@]}
do
  if [[ ${zookeeper_server} == *${HOSTNAME_POSTFIX} ]]
  then
    zookeeper_server_host_name=${zookeeper_server}
  else
    zookeeper_server_host_name=${zookeeper_server}${HOSTNAME_POSTFIX}
  fi
  zookeeper_server_ip=$(getent ahosts ${zookeeper_server_host_name} | grep STREAM | awk '{print $1}')
  echo "server.${id}=${zookeeper_server_ip}:${ZOOKEEPER_FOLLOWER_PORT}:${ZOOKEEPER_ELECTION_PORT}" >> ${ZOOKEEPER_CONF_FILE}
  ((id=${id}+1))
done
