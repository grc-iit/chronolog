#!/bin/bash

CHRONOLOG_ROOT_DIR="/home/${USER}/chronolog"
BIN_DIR="${CHRONOLOG_ROOT_DIR}/bin"
LIB_DIR="${CHRONOLOG_ROOT_DIR}/lib"
CONF_DIR="${CHRONOLOG_ROOT_DIR}/conf"
VISOR_BIN_FILE_NAME="chronovisor_server_test"
KEEPER_BIN_FILE_NAME="chronokeeper_test"
CLIENT_BIN_FILE_NAME="client_lib_metadata_rpc_test"
VISOR_BIN="${BIN_DIR}/${VISOR_BIN_FILE_NAME}"
KEEPER_BIN="${BIN_DIR}/${KEEPER_BIN_FILE_NAME}"
CLIENT_BIN="${BIN_DIR}/${CLIENT_BIN_FILE_NAME}"
VISOR_HOSTS="${CONF_DIR}/hosts_visor"
KEEPER_HOSTS="${CONF_DIR}/hosts_keeper"
CLIENT_HOSTS="${CONF_DIR}/hosts_client"
CONF_FILE="${CONF_DIR}/default_conf.json"

function check_hosts_files() {
  echo "Checking hosts files..."
  if [[ ! -f ${VISOR_HOSTS} ]]
  then
    echo ${VISOR_HOSTS} host file does not exist, exiting ...
    exit 1
  fi

  if [[ ! -f ${KEEPER_HOSTS} ]]
  then
    echo ${KEEPER_HOSTS} host file does not exist, exiting ...
    exit 1
  fi

  if [[ ! -f ${CLIENT_HOSTS} ]]
  then
    echo ${CLIENT_HOSTS} host file does not exist, exiting ...
    exit 1
  fi
}

function check_bin_files() {
  echo "Checking binary files..."
  if [[ ! -f ${VISOR_BIN} ]]
  then
    echo ${VISOR_BIN} executable file does not exist, exiting ...
    exit 1
  fi

  if [[ ! -f ${KEEPER_BIN} ]]
  then
    echo ${KEEPER_BIN} executable file does not exist, exiting ...
    exit 1
  fi

  if [[ ! -f ${CLIENT_BIN} ]]
  then
    echo ${CLIENT_BIN} executable file does not exist, exiting ...
    exit 1
  fi
}

function check_conf_files() {
  echo "Checking configuration files..."
  if [[ ! -f ${CONF_FILE} ]]
  then
    echo ${CONF_FILE} configuration file does not exist, exiting ...
    exit 1
  fi
}

function deploy() {
  check_hosts_files

  check_bin_files

  check_conf_files

  echo "Deploying..."

  # launch Visor
  mpssh -f ${VISOR_HOSTS} "nohup ${VISOR_BIN} ${CONF_FILE} > /dev/null 2>&1 &"

  # launch Keeper
  mpssh -f ${KEEPER_HOSTS} "nohup ${KEEPER_BIN} ${CONF_FILE} > /dev/null 2>&1 &"

  # launch Client
  mpssh -f ${CLIENT_HOSTS} "${CLIENT_BIN} ${CONF_FILE}"
}

function reset() {
  echo "Resetting..."

  # kill Visor
  mpssh -f ${VISOR_HOSTS} "pkill --signal 9 -f ${VISOR_BIN_FILE_NAME}; ps aux | grep chrono"

  # kill Keeper
  mpssh -f ${KEEPER_HOSTS} "pkill --signal 9 -f ${KEEPER_BIN_FILE_NAME}; ps aux | grep chrono"

  # kill Client
  mpssh -f ${CLIENT_HOSTS} "pkill --signal 9 -f ${CLIENT_BIN_FILE_NAME}; ps aux | grep chrono"
}

args=("$@")

if [[ $# -eq 0 || "$1" == "deploy" ]]
then
  deploy
elif [[ $1 == "reset" ]]
then
  reset
else
  echo "Invalid command line argument"
fi
