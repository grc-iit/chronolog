#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

host_id=`hostname | cut -d'-' -f3`
num_msgs=$1
msg_size=$2
num_partitions=$3

${KAFKA_ROOT_DIR}/bin/kafka-run-class.sh kafka.tools.EndToEndLatency ${kafka_bootstrap_server} end-to-end-latency-test-${num_partitions}-${host_id} ${num_msgs} 1 ${msg_size}
