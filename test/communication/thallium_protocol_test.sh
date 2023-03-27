#!/bin/bash

help() {
  echo "Usage: $0 [-j job_id] [-s server_bin] [-c client_bin] [-n num_clients]"
  echo ""
  echo "  -h    Show this help message and exit"
  echo "  -j    Job ID to get node info (default: first job from squeue)"
  echo "  -s    Server executable binary (default: thallium_server)"
  echo "  -c    Client executable binary (default: thallium_client)"
  echo "  -n    Number of clients (default: 1)"
  echo ""
}

WORK_DIR="/mnt/common/kfeng/CLionProjects/ChronoLog_dev/decouple_conf_manager/ChronoLog/build/test/communication"
SERVER_BIN="thallium_server"
CLIENT_BIN="thallium_client"
num_clients=1
node_list=$(squeue | grep $USER | head -1 | awk '{print $NF}')
while getopts "j:c:s:n:h" opt; do
  case ${opt} in
    j)
      job_id=${OPTARG}
      node_list=$(scontrol show job ${job_id} | grep "\<NodeList" | cut -d'=' -f2)
      ;;
    c)
      client_bin_path=$OPTARG
      WORK_DIR=$(dirname ${client_bin_path} | xargs realpath)
      CLIENT_BIN=$(basename ${client_bin_path})
      ;;
    s)
      server_bin_path=$OPTARG
      WORK_DIR=$(dirname ${server_bin_path} | xargs realpath)
      SERVER_BIN=$(basename ${server_bin_path})
      ;;
    n)
      num_clients=$OPTARG
      ;;
    h)
      help | fold -w 120 -s | column -t -s $'\t'
      exit 0
      ;;
    \?)
      echo "Invalid option: -$OPTARG" >&2
      exit 1
      ;;
    :)
      echo "Option -$OPTARG requires an argument." >&2
      exit 1
      ;;
  esac
done
prefix=${node_list%[*}
regex="${prefix}\[([0-9]{2})-([0-9]{2})\]"
if [[ ${node_list} =~ $regex ]]
then
  ext_node_list="(${prefix}{${BASH_REMATCH[1]}..${BASH_REMATCH[2]}})"
fi
eval "node_array=${ext_node_list}"
SERVER_HOST="${node_array[0]}"
CLIENT_HOSTS="${node_array[@]:1:num_clients}"
SERVER_IP=$(ssh ${SERVER_HOST} "ip addr show | grep 172.25 | awk '{print \$2}' | sed 's/^[[:space:]]*//' | cut -d' ' -f2 | cut -d'/' -f1") # only works for Ares
SERVER_PORT=5555
MODES=("sendrecv" "rdma")
PROTOS=("sockets" "tcp" "verbs")
SIZE=134217728
REP=1

echo "${#node_array[@]} nodes in total, use ${SERVER_HOST} as server and ${CLIENT_HOSTS[@]} as client(s) ..."

for proto in "${PROTOS[@]}"
do
  for mode in "${MODES[@]}"
  do
    addr=ofi+${proto}://${SERVER_IP}:${SERVER_PORT}
    echo server address: ${addr}
    echo -e "\033[0;33mLaunching server ...\033[0m"
    ssh ${SERVER_HOST} killall -9 ${SERVER_BIN} > /dev/null 2>&1
    ssh ${SERVER_HOST} "ABT_THREAD_STACKSIZE=2097152 ${WORK_DIR}/${SERVER_BIN} \"${addr}\" 1 1" &
    sleep 3
    pid=$!
    echo -e "\033[0;33mLaunching client ...\033[0m"
    for client_host in ${CLIENT_HOSTS[@]}
    do
      ssh ${client_host} killall -9 ${CLIENT_BIN} > /dev/null 2>&1
    done
    overall_ret=0
    for client_host in ${CLIENT_HOSTS[@]}
    do
      {
        timeout -s 9 10 ssh ${client_host} ${WORK_DIR}/${CLIENT_BIN} \"${addr}\" ${mode} ${SIZE} ${REP}
        ret=$?
        if [[ ${ret} -ne 0 ]]
        then
          overall_ret=1
        fi
      } &
    done
    kill -9 ${pid}
    if [[ ${ret} -eq 0 ]]
    then
      echo -e "\033[0;32mTest of procotol ${proto} in mode ${mode} PASSES\033[0m"
    else
      echo -e "\033[0;31mTest of procotol ${proto} in mode ${mode} FAILS\033[0m"
    fi
  done
done
