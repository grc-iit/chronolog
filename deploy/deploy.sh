#!/bin/bash

CHRONOLOG_ROOT_DIR="/home/${USER}/chronolog"
BIN_DIR="${CHRONOLOG_ROOT_DIR}/bin"
LIB_DIR="${CHRONOLOG_ROOT_DIR}/lib"
CONF_DIR="${CHRONOLOG_ROOT_DIR}/conf"
VISOR_BIN="${BIN_DIR}/chronovisor_server"
KEEPER_BIN="${BIN_DIR}/chrono_keeper"
CLIENT_BIN="${BIN_DIR}/storyteller_test"
VISOR_HOSTS="${BIN_DIR}/hosts_visor"
KEEPER_HOSTS="${BIN_DIR}/hosts_keeper"
CLIENT_HOSTS="${BIN_DIR}/hosts_client"
SERVER_USERNAME="chronolog"
CLIENT_USERNAMES=("chl_user1", "chl_user2", "chl_user3", "chl_user4")

# set setgid bit
#if [[ $(chmod g+s ${VISOR_BIN}) -ne 0 || $(chmod g+s ${KEEPER_BIN}) -ne 0 || $(chmod g+s ${CLIENT_BIN}) -ne 0 ]]
#then
  #echo setgid bit fails, exiting ...
  #exit 1
#fi

## double check
#if [[ ! -g ${VISOR_BIN} || ! -g ${KEEPER_BIN} || ! -g ${CLIENT_BIN} ]]
#then
  #echo setgid bit is not set, exiting ...
  #exit 1
#fi

# launch Visor
mpssh -f ${VISOR_HOSTS} "cd ${BIN_DIR}; sudo -u ${SERVER_USERNAME} ./chronovisor_server_test ${CONF_DIR}/default_conf.json"

# launch Keeper
mpssh -f ${KEEPER_HOSTS} "sudo -u ${SERVER_USERNAME} \"LD_LIBRARY_PATH=${LIB_DIR} ${KEEPER_BIN} ${CONF_DIR}/default_conf.json\""

# launch Client
for client_username in ${CLIENT_USERNAMES[@]}
do
  mpssh -f ${CLIENT_HOSTS} "sudo -u ${client_username} \"LD_LIBRARY_PATH=${LIB_DIR} ${CLIENT_BIN} ${CONF_DIR}/default_conf.json\""
done
