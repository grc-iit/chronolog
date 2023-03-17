#!/bin/bash

SERVER_BIN="/mnt/common/kfeng/CLionProjects/ChronoLog_dev/decouple_conf_manager/ChronoLog/build/test/communication/thallium_server"
CLIENT_BIN="/mnt/common/kfeng/CLionProjects/ChronoLog_dev/decouple_conf_manager/ChronoLog/build/test/communication/thallium_client"
SERVER_HOST="ares-comp-03"
CLIENT_HOST="ares-comp-04"
SERVER_IP=$(ssh ${SERVER_HOST} "ip addr show | grep 172.25 | awk '{print $2}' | sed 's/^[[:space:]]*//' | cut -d' ' -f2 | cut -d'/' -f1") # only works for Ares
SERVER_PORT=5555
MODES=("sendrecv" "rdma")
PROTOS=("sockets" "tcp" "verbs")
SIZE=134217728
REP=1

for proto in "${PROTOS[@]}"
do
  for mode in "${MODES[@]}"
  do
    addr=ofi+${proto}://${SERVER_IP}:${SERVER_PORT}
    echo server address: ${addr}
    echo -e "\033[0;33mLaunching server ...\033[0m"
    ssh ${SERVER_HOST} killall -9 thallium_server > /dev/null 2>&1
    ssh ${SERVER_HOST} "ABT_THREAD_STACKSIZE=2097152 ${SERVER_BIN} \"${addr}\" 1 1" &
    sleep 3
    pid=$!
    echo -e "\033[0;33mLaunching client ...\033[0m"
    ssh ${CLIENT_HOST} killall -9 thallium_client > /dev/null 2>&1
    timeout -s 9 10 ssh ${CLIENT_HOST} ${CLIENT_BIN} \"${addr}\" ${mode} ${SIZE} ${REP}
    ret=$?
    kill -9 ${pid}
    if [[ ${ret} -eq 0 ]]
    then
      echo -e "\033[0;32mTest of procotol ${proto} in mode ${mode} PASSES\033[0m"
    else
      echo -e "\033[0;31mTest of procotol ${proto} in mode ${mode} FAILS\033[0m"
    fi
  done
done
