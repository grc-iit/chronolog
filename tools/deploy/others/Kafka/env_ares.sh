#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
KAFKA_ROOT_DIR=/mnt/common/kfeng/pkg_src/kafka_2.13-2.8.0
KAFKA_SCRIPTS_DIR=${KAFKA_ROOT_DIR}/scripts

HOSTNAME=`head -1 ${CWD}/servers`
HOSTNAME_POSTFIX=-40g
if [[ ${HOSTNAME} == *comp* ]]
then
  drive="nvme"
elif [[ ${HOSTNAME} == *stor* ]]
then
  drive="hdd"
fi

# Zookeeper
ZOOKEEPER_DATA_DIR=/mnt/${drive}/kfeng/zookeeper
ZOOKEEPER_SERVERS_HOSTFILE=${CWD}/servers
ZOOKEEPER_N_SERVERS=`wc -l ${ZOOKEEPER_SERVERS_HOSTFILE} | cut -d' ' -f1`
ZOOKEEPER_SERVERS=`cat ${ZOOKEEPER_SERVERS_HOSTFILE} | awk '{print $1}'`
ZOOKEEPER_CONF_FILE=${KAFKA_SCRIPTS_DIR}/zookeeper.properties
ZOOKEEPER_CLIENT_PORT=2181
ZOOKEEPER_FOLLOWER_PORT=2888
ZOOKEEPER_ELECTION_PORT=3888
ZOOKEEPER_HEAP_SIZE_IN_MB=2048
kafka_zookeeper_connect=""
for zookeeper_server in ${ZOOKEEPER_SERVERS[@]}
do
  if [[ ${zookeeper_server} == *${HOSTNAME_POSTFIX} ]]
  then
    zookeeper_server_host_name=${zookeeper_server}
  else
    zookeeper_server_host_name=${zookeeper_server}${HOSTNAME_POSTFIX}
  fi
  zookeeper_server_ip=$(getent ahosts ${zookeeper_server_host_name} | grep STREAM | awk '{print $1}')
  if [[ ! -z "${kafka_zookeeper_connect}" ]]
  then
    kafka_zookeeper_connect="${kafka_zookeeper_connect},"
  fi
  kafka_zookeeper_connect="${kafka_zookeeper_connect}${zookeeper_server_ip}:${ZOOKEEPER_CLIENT_PORT}"
done

# Kafka
KAFKA_LOG_DIR=/mnt/${drive}/kfeng/kafka
KAFKA_SERVERS_HOSTFILE=${CWD}/servers
KAFKA_CLIENTS_HOSTFILE=${CWD}/clients
KAFKA_N_SERVERS=`wc -l ${KAFKA_SERVERS_HOSTFILE} | cut -d' ' -f1`
KAFKA_N_CLIENTS=`wc -l ${KAFKA_CLIENTS_HOSTFILE} | cut -d' ' -f1`
KAFKA_SERVERS=`cat ${KAFKA_SERVERS_HOSTFILE} | awk '{print $1}'`
KAFKA_CLIENTS=`cat ${KAFKA_CLIENTS_HOSTFILE} | awk '{print $1}'`
KAFKA_CONF_FILE=${KAFKA_LOG_DIR}/server.properties
KAFKA_PORT=9092
KAFKA_HEAP_SIZE_IN_MB=8192
kafka_bootstrap_server=""
for kafka_server in ${KAFKA_SERVERS[@]}
do
  if [[ ${kafka_server} == *${HOSTNAME_POSTFIX} ]]
  then
    kafka_server_host_name=${kafka_server}
  else
    kafka_server_host_name=${kafka_server}${HOSTNAME_POSTFIX}
  fi
  kafka_server_ip=$(getent ahosts ${kafka_server_host_name} | grep STREAM | awk '{print $1}')
  if [[ ! -z "${kafka_bootstrap_server}" ]]
  then
    kafka_bootstrap_server="${kafka_bootstrap_server},"
  fi
  kafka_bootstrap_server="${kafka_bootstrap_server}${kafka_server_ip}:${KAFKA_PORT}"
done
