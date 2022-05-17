#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

# Stop Kafka
mpssh -f ${KAFKA_SERVERS_HOSTFILE} "jps -l | grep Kafka | cut -d' ' -f1 | xargs -r kill -9"
