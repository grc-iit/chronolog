#!/bin/bash

if [ $# -lt 3 ];
then
	echo "USAGE: $0 NUM_MSG MSG_SIZE_BYTE NUM_PARTITION"
	exit 1
else
  num_msgs=$1
  msg_size=$2
  num_partitions=$3
fi

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

for kafka_client in ${KAFKA_CLIENTS[@]}
do
  host_id=`echo ${kafka_client} | cut -d'-' -f3`
  ${KAFKA_ROOT_DIR}/bin/kafka-topics.sh --create --topic end-to-end-latency-test-${num_partitions}-${host_id} --partitions ${num_partitions} --config retention.ms=86400000 --bootstrap-server ${kafka_bootstrap_server}
done

mpssh -f ${CWD}/clients "${KAFKA_ROOT_DIR}/scripts/e2elat.sh"
