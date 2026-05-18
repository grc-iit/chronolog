#!/bin/bash

#if [ $# -lt 3 ];
#then
	#echo "USAGE: $0 NUM_MSG MSG_SIZE_BYTE NUM_PARTITION"
	#exit 1
#else
  #num_msgs=$1
  #msg_size=$2
  #num_partitions=$3
#fi

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
NUM_CLIENTS=(8 16 32 40 80 160)
NUM_MSGS=(1000)
MSG_SIZES=(512 2048 8192 32768 131072 524288)
NUM_PARTITIONS=(1 4 16 64 256)
REP=3
#NUM_CLIENTS=(4 80)
#NUM_MSGS=(1000)
#MSG_SIZES=(512 524288)
#NUM_PARTITIONS=(1 256)
#REP=1

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

rm -f read_write_test.log
for rep in `seq 1 ${REP}`
do
  for num_clients in ${NUM_CLIENTS[@]}
  do
    truncate -s 0 ${CWD}/client_hosts
    idx=1
    for i in `seq 1 ${num_clients}`
    do
      sed "${idx}q;d" ${CWD}/clients >> ${CWD}/client_hosts
      ((idx=idx+1))
      if [[ ${idx} > ${KAFKA_N_CLIENTS} ]]
      then
        idx=1
      fi
    done
    for num_msgs in ${NUM_MSGS[@]}
    do
      for msg_size in ${MSG_SIZES[@]}
      do
        for num_partitions in ${NUM_PARTITIONS[@]}
        do
          echo "######################"               | tee -a read_write_test.log
          echo "Testing rep ${rep} ..."               | tee -a read_write_test.log
          echo "num_clients: ${num_clients}"          | tee -a read_write_test.log
          echo "num_msgs: ${num_msgs}"                | tee -a read_write_test.log
          echo "msg_size: ${msg_size}"                | tee -a read_write_test.log
          echo "num_partitions: ${num_partitions}"    | tee -a read_write_test.log
          echo "Write:"                               | tee -a read_write_test.log
          ${KAFKA_ROOT_DIR}/bin/kafka-topics.sh --create --topic simple-perf-test-${num_partitions} --partitions ${num_partitions} --config retention.ms=86400000 --bootstrap-server ${kafka_bootstrap_server} >> read_write_test.log

          write_output=`mpssh -p ${num_clients} -f ${CWD}/client_hosts "${KAFKA_ROOT_DIR}/bin/kafka-producer-perf-test.sh --topic simple-perf-test-${num_partitions} --throughput -1 --num-records ${num_msgs} --record-size ${msg_size} --producer-props acks=all bootstrap.servers=${kafka_bootstrap_server}"`
          echo "${write_output}" >> read_write_test.log
          echo "${write_output}" | grep 99.9th | awk '{print $8}' | cut -d'(' -f2 | awk '{ SUM += $1; print $1 } END { print "Aggr BW: ", SUM }'

          mpssh -f ${CWD}/clients "sudo fm" > /dev/null 2>&1
          mpssh -f ${CWD}/servers "sudo fm" > /dev/null 2>&1

          echo "Read:"
          read_output=`mpssh -f ${CWD}/client_hosts "${KAFKA_ROOT_DIR}/bin/kafka-consumer-perf-test.sh --topic simple-perf-test-${num_partitions} --messages ${num_msgs} --bootstrap-server=${kafka_bootstrap_server} | jq -R .|jq -sr 'map(./\",\")|transpose|map(join(\": \"))[]'"`
          echo "${read_output}" >> read_write_test.log
          echo "${read_output}" | grep '\sMB.sec' | awk '{print $4}' | awk '{ SUM += $1; print $1 } END { print "Aggr BW: ", SUM }'

          ${KAFKA_ROOT_DIR}/bin/kafka-topics.sh --delete --topic simple-perf-test-${num_partitions} --bootstrap-server ${kafka_bootstrap_server} >> read_write_test.log

          sleep 10
        done
      done
    done
  done
done
