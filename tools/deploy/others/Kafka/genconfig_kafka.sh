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
