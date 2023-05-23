#!/bin/bash

CHRONOLOG_ROOT_DIR="/home/${USER}/chronolog"
BIN_DIR="${CHRONOLOG_ROOT_DIR}/bin"
LIB_DIR="${CHRONOLOG_ROOT_DIR}/lib"
CONF_DIR="${CHRONOLOG_ROOT_DIR}/conf"
VISOR_BIN="${BIN_DIR}/chronovisor_server_test"
KEEPER_BIN="${BIN_DIR}/chronokeeper_test"
CLIENT_BIN="${BIN_DIR}/client_lib_metadata_rpc_test"
VISOR_HOSTS="${CONF_DIR}/hosts_visor"
KEEPER_HOSTS="${CONF_DIR}/hosts_keeper"
CLIENT_HOSTS="${CONF_DIR}/hosts_client"
CONF_FILE="${CONF_DIR}/default_conf.json"

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

if [[ ! -f ${CONF_FILE} ]]
then
  echo ${CONF_FILE} configuration file does not exist, exiting ...
  exit 1
fi

# launch Visor
mpssh -f ${VISOR_HOSTS} "${VISOR_BIN} ${CONF_FILE} &"

# launch Keeper
mpssh -f ${KEEPER_HOSTS} "${KEEPER_BIN} ${CONF_FILE} &"

# launch Client
mpssh -f ${CLIENT_HOSTS} "${CLIENT_BIN} ${CONF_FILE}"
