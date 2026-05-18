#!/bin/bash

mpssh > /dev/null 2>&1 || { echo >&2 "mpssh is not found.  Aborting."; exit 1; }

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
SERVER_LOCAL_PATH="/home/kfeng/mongodb"
CLIENT_LOCAL_PATH="/home/kfeng/mongodb"
TMPFS_PATH="/dev/shm"
mongod_config_path=${SERVER_LOCAL_PATH}/mongod_config
mongos_local_path=${CLIENT_LOCAL_PATH}/mongos
mongod_shard_path=${SERVER_LOCAL_PATH}/mongod_shard
mongos_diag_data_path=${TMPFS_PATH}/mongos.diagnostic.data
HOSTNAME_POSTFIX="-ib"
MONGOD_CONFIG_CONF_FILE=mongod_config.conf
MONGOD_CONFIG_LOG_FILE=mongod_config.log
MONGOD_SHARD_CONF_FILE=mongod_shard.conf
MONGOD_SHARD_LOG_FILE=mongod_shard.log
MONGOS_CONF_FILE=mongos.conf
MONGOS_LOG_FILE=mongos.log
MONGO_PORT=27017
SHARD_BASE_PORT=27100
CONFIG_REPL_NAME=replconfig01
SHARD_REPL_NAME=shard
CONFIG_SERVER_COUNT=2
SHARD_SERVER_COUNT=`cat ${CWD}/servers | wc -l`
SHARD_COPY_COUNT=1
ROUTER_SERVER_COUNT=`cat ${CWD}/clients | wc -l`
