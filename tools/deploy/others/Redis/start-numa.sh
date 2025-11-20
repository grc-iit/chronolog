#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ -f ${CWD}/env.sh ]
then
  source ${CWD}/env.sh
else
  echo "env.sh does not exist, quiting ..."
  exit
fi

function version_gt() { test "$(printf '%s\n' "$@" | sort -V | head -n 1)" != "$1"; }

n_server=`cat ${CWD}/servers | wc -l`
if [ $((n_server%2)) -ne 0 ]
then
  echo "Even number of servers are required, exiting ..."
  exit
fi

# Prepare configuration for each server
echo -e "${GREEN}Preparing Redis cluster configuration files ...${NC}"
i=0
for server in ${SERVERS[@]}
do
  server_ip=$(getent ahosts $server$HOSTNAME_POSTFIX | grep STREAM | awk '{print $1}')
  ((port=$PORT_BASE+$i))
  mkdir -p $port
  rm -rf $port/$CONF_FILE
  echo "port $port" >> $port/$CONF_FILE
  echo "cluster-enabled yes" >> $port/$CONF_FILE
  echo "cluster-config-file nodes.conf" >> $port/$CONF_FILE
  echo "cluster-node-timeout 5000" >> $port/$CONF_FILE
  echo "appendonly yes" >> $port/$CONF_FILE
  #echo "appendfsync always" >> $port/$CONF_FILE
  echo "protected-mode no" >> $port/$CONF_FILE
  echo "logfile $LOCAL_DIR/$port/file.log" >> $port/$CONF_FILE
  ((i=i+1))
done

# Copy configuration files to local directories on all servers
echo -e "${GREEN}Copying Redis cluster configuration files ...${NC}"
i=0
for server in ${SERVERS[@]}
do
  ((port=$PORT_BASE+$i))
  echo Copying configuration directory $port to $server ...
  rsync -qraz ${CWD}/$port $server:$LOCAL_DIR/ &
  ((i=i+1))
done
wait

# Start server
echo -e "${GREEN}Starting Redis ...${NC}"
i=0
for server in ${SERVERS[@]}
do
  ((port=$PORT_BASE+$i))
  echo Starting redis on $server:$port ...
  ssh $server "sh -c \"cd $LOCAL_DIR/$port; $REDIS_DIR/src/redis-server ./$CONF_FILE > /dev/null 2>&1 &\"" &
  ssh $server "pgrep redis-server | xargs taskset -cp 4,5,6,7" &
  ((i=i+1))
done
wait

# Verify server
echo -e "${GREEN}Verifying Redis cluster servers ...${NC}"
mpssh -f ${CWD}/servers 'pgrep -l redis-server'

# Connect servers
# for Redis 5 the command should be like redis-cli --cluster create 127.0.0.1:7000 127.0.0.1:7001 --cluster-replicas 1
# for Redis 3 and 4, the command looks like ./redis-trib.rb create --replicas 1 127.0.0.1:7000 127.0.0.1:7001
echo -e "${GREEN}Connecting Redis cluster servers ...${NC}"
i=0
if version_gt $REDIS_VER "5.0"
then
  echo "Redis 5.x, using redis-cli ..."
  cmd="$REDIS_DIR/src/redis-cli --cluster create "
else
  echo "Redis 3.x/4.x, using redis-trib.rb ..."
  cmd="$REDIS_DIR/src/redis-trib.rb create --replicas 0 "
fi

for server in ${SERVERS[@]}
do
  server_ip=$(getent ahosts $server$HOSTNAME_POSTFIX | grep STREAM | awk '{print $1}')
  ((port=$PORT_BASE+$i))
  cmd="${cmd}${server_ip}:${port} "
  ((i=i+1))
done
if version_gt $REDIS_VER "5.0"
then
  cmd="${cmd}--cluster-replicas 1"
fi
echo yes | $cmd

# Check cluster nodes
echo -e "${GREEN}Checking Redis cluster nodes ...${NC}"
first_server=`head -1 ${CWD}/servers`
cmd="$REDIS_DIR/src/redis-cli -c -h ${first_server}${HOSTNAME_POSTFIX} -p ${PORT_BASE} cluster nodes"
$cmd | sort -k9 -n
echo -e "${GREEN}Redis is started${NC}"
