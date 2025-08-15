#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
LOCAL_DIR=/mnt/hdd/kfeng/redis
REDIS_DIR=~/pkg_src/redis-3.2.13
REDIS_VER=`${REDIS_DIR}/src/redis-server -v | awk '{print $3}' | cut -d'=' -f2`
CONF_FILE=redis.conf
HOSTNAME_POSTFIX=-40g
SERVERS=`cat ${CWD}/servers | awk '{print $1}'`
PORT_BASE=7000
