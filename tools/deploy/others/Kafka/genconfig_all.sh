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
#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi
# Kafka
mpssh -f ${KAFKA_SERVERS_HOSTFILE} "mkdir -p ${KAFKA_LOG_DIR}"
broker_id=1
for kafka_server in ${KAFKA_SERVERS[@]}
do
  kafka_conf_file=${CWD}/server.properties.${broker_id}
  cp ${KAFKA_ROOT_DIR}/config/server.properties ${kafka_conf_file}
  kafka_log_dir_esc=$(sed 's/\//\\&/g' <<<"${KAFKA_LOG_DIR}")
  sed -i "s/^log.dirs=.*/log.dirs=${kafka_log_dir_esc}/" ${kafka_conf_file}
  if [[ ${kafka_server} == *${HOSTNAME_POSTFIX} ]]
  then
    kafka_server_host_name=${kafka_server}
  else
    kafka_server_host_name=${kafka_server}${HOSTNAME_POSTFIX}
  fi
  kafka_server_ip=$(getent ahosts ${kafka_server_host_name} | grep STREAM | awk '{print $1}')
  sed -Ei "s/^(\#)?listeners=PLAINTEXT.*/listeners=PLAINTEXT:\/\/${kafka_server_ip}:${KAFKA_PORT}/" ${kafka_conf_file}
  sed -i "s/^host\.name=.*/host\.name=${kafka_server_ip}/" ${kafka_conf_file}
  sed -i "s/^broker\.id=.*/broker\.id=${broker_id}/" ${kafka_conf_file}
  ((broker_id=${broker_id}+1))

  sed -i "s/^zookeeper\.connect=.*/zookeeper\.connect=${kafka_zookeeper_connect}/" ${kafka_conf_file}

  #echo "num.network.threads=10" >> ${kafka_conf_file}
  #echo "num.io.threads=32" >> ${kafka_conf_file}
  #echo "num.background.threas=20" >> ${kafka_conf_file}

  rsync -az ${kafka_conf_file} ${kafka_server}:${KAFKA_CONF_FILE} &
done
wait
