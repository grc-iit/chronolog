#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

# Start Zookeeper
id=1
for zookeeper_server in ${ZOOKEEPER_SERVERS[@]}
do
  ssh ${zookeeper_server} "mkdir -p ${ZOOKEEPER_DATA_DIR} && echo ${id} > ${ZOOKEEPER_DATA_DIR}/myid" &
  ((id=${id}+1))
done
wait

if [[ -f ${ZOOKEEPER_CONF_FILE} ]]
then
  mpssh -f ${ZOOKEEPER_SERVERS_HOSTFILE} "KAFKA_JMX_OPTS='-Djava.net.preferIPv4Stack=true' KAFKA_HEAP_OPTS='-Xmx${ZOOKEEPER_HEAP_SIZE_IN_MB}m -Xms${ZOOKEEPER_HEAP_SIZE_IN_MB}m' ${KAFKA_ROOT_DIR}/bin/zookeeper-server-start.sh -daemon ${ZOOKEEPER_CONF_FILE}"
  sleep 5
  mpssh -f ${ZOOKEEPER_SERVERS_HOSTFILE} "jps" | sort -u
else
  echo "Zookeeper configuration file ${ZOOKEEPER_CONF_FILE} missing, exiting ..."
  exit
fi
#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

# Start Kafka
num_conf_files=`mpssh -f servers 'ls -l /mnt/nvme/kfeng/kafka/server.properties' 2>&1 | grep server | grep -v cannot | wc -l`
if [[ ${num_conf_files} == ${KAFKA_N_SERVERS} ]]
then
  mpssh -f ${KAFKA_SERVERS_HOSTFILE} "mkdir -p ${KAFKA_LOG_DIR}"
  mpssh -f ${KAFKA_SERVERS_HOSTFILE} "KAFKA_JMX_OPTS='-Djava.net.preferIPv4Stack=true' KAFKA_HEAP_OPTS='-Xmx${KAFKA_HEAP_SIZE_IN_MB}m -Xms${KAFKA_HEAP_SIZE_IN_MB}m' ${KAFKA_ROOT_DIR}/bin/kafka-server-start.sh -daemon ${KAFKA_CONF_FILE}"
  sleep 5
  mpssh -f ${KAFKA_SERVERS_HOSTFILE} "jps" | sort -u
else
  echo "${num_conf_files} Kafka configuration file found, ${KAFKA_N_SERVERS} is expected, exiting ..."
  exit
fi
