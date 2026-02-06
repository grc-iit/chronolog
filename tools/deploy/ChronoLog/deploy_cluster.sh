#!/bin/bash
# Deployment helper that runs directly from an installed ChronoLog tree.
# Supports starting, stopping, and cleaning a cluster/Slurm single-user deployment
# without requiring the source repository.

set -e

# Define some colors
ERR='\033[7;37m\033[41m'
INFO='\033[7;49m\033[92m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

# Basics
USER=$(whoami)

# Default values
BUILD_TYPE="Release"
NUM_RECORDING_GROUP=1

# Directories (with defaults based on script location)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="$(realpath -m "${SCRIPT_DIR}/..")"
LIB_DIR="${WORK_DIR}/lib"
CONF_DIR="${WORK_DIR}/conf"
BIN_DIR="${WORK_DIR}/bin"
MONITOR_DIR="${WORK_DIR}/monitor"
OUTPUT_DIR="${WORK_DIR}/output"

# Binary names
VISOR_BIN_FILE_NAME="chronovisor_server"
GRAPHER_BIN_FILE_NAME="chrono_grapher"
KEEPER_BIN_FILE_NAME="chrono_keeper"
PLAYER_BIN_FILE_NAME="chrono_player"

# Binary paths (with defaults)
VISOR_BIN="${BIN_DIR}/${VISOR_BIN_FILE_NAME}"
GRAPHER_BIN="${BIN_DIR}/${GRAPHER_BIN_FILE_NAME}"
KEEPER_BIN="${BIN_DIR}/${KEEPER_BIN_FILE_NAME}"
PLAYER_BIN="${BIN_DIR}/${PLAYER_BIN_FILE_NAME}"
VISOR_BIN_DIR="${BIN_DIR}"
GRAPHER_BIN_DIR="${BIN_DIR}"
KEEPER_BIN_DIR="${BIN_DIR}"
PLAYER_BIN_DIR="${BIN_DIR}"

# Configuration file and component-specific conf arguments (with defaults)
CONF_FILE="${WORK_DIR}/conf/default_conf.json"
CLIENT_CONF_FILE="${WORK_DIR}/conf/default_client_conf.json"
VISOR_ARGS="--config ${CONF_FILE}"
GRAPHER_ARGS="--config ${CONF_FILE}"
KEEPER_ARGS="--config ${CONF_FILE}"
PLAYER_ARGS="--config ${CONF_FILE}"

# Hosts files (with defaults)
VISOR_HOSTS="${WORK_DIR}/conf/hosts_visor"
GRAPHER_HOSTS="${WORK_DIR}/conf/hosts_grapher"
KEEPER_HOSTS="${WORK_DIR}/conf/hosts_keeper"
PLAYER_HOSTS="${WORK_DIR}/conf/hosts_player"

# Cluster specific settings
HOSTNAME_HS_NET_SUFFIX=""
JOB_ID=""

# Operation flags
start=false
stop=false
clean=false

# Verbose flag
verbose=false

usage() {
  echo "Usage: $0 [options]"
  echo ""
  echo "Execution Modes (Select only ONE):"
  echo "  -d|--start          Start ChronoLog deployment"
  echo "  -s|--stop           Stop ChronoLog deployment"
  echo "  -c|--clean          Clean ChronoLog logging artifacts and generated files"
  echo ""
  echo "Deployment Options:"
  echo "  -r|--record-groups <number>      Number of RecordingGroups/ChronoGraphers"
  echo "  -j|--job-id <number>             Slurm job ID to derive hosts (optional)"
  echo "  -q|--visor-hosts <path>          Hosts file for ChronoVisor (default: work_dir/conf/hosts_visor)"
  echo "  -k|--grapher-hosts <path>        Hosts file for ChronoGrapher (default: work_dir/conf/hosts_grapher)"
  echo "  -o|--keeper-hosts <path>         Hosts file for ChronoKeeper (default: work_dir/conf/hosts_keeper)"
  echo ""
  echo "Directory Settings:"
  echo "  -w|--work-dir <path>             Working directory (default: script_dir/..)"
  echo "  -m|--monitor-dir <path>          Monitoring directory (default: work_dir/monitor)"
  echo "  -u|--output-dir <path>           Output directory (default: work_dir/output)"
  echo ""
  echo "Binary Paths:"
  echo "  -v|--visor-bin <path>            ChronoVisor binary (default: work_dir/bin/chronovisor_server)"
  echo "  -g|--grapher-bin <path>          ChronoGrapher binary (default: work_dir/bin/chrono_grapher)"
  echo "  -p|--keeper-bin <path>           ChronoKeeper binary (default: work_dir/bin/chrono_keeper)"
  echo "  -a|--player-bin <path>           ChronoPlayer binary (default: work_dir/bin/chrono_player)"
  echo ""
  echo "Configuration Settings:"
  echo "  -f|--conf-file <path>            Main configuration file (default: work_dir/conf/default_conf.json)"
  echo "  -n|--client-conf-file <path>     Client configuration file (default: work_dir/conf/default_client_conf.json)"
  echo ""
  echo "Miscellaneous Options:"
  echo "  -e|--verbose                     Enable verbose output"
  echo ""
  echo "Examples:"
  echo "  Start using defaults from the installed tree:"
  echo "    $0 --start"
  echo ""
  echo "  Start using hosts from a Slurm job with two recording groups:"
  echo "    $0 --start --job-id <job_id> --record-groups 2"
  echo ""
  echo "  Stop deployment using defaults:"
  echo "    $0 --stop"
  echo ""
  exit 1
}

check_dependencies() {
  local dependencies=("jq" "parallel-ssh" "ssh" "ldd" "nohup" "pkill" "readlink" "realpath" "chrpath")

  echo -e "${DEBUG}Checking required dependencies...${NC}"
  for dep in "${dependencies[@]}"; do
    if ! command -v "$dep" &>/dev/null; then
      echo -e "${ERR}Dependency $dep is not installed. Please install it and try again.${NC}"
      exit 1
    fi
  done
  echo -e "${DEBUG}All required dependencies are installed.${NC}"
}

check_file_existence() {
  local file=$1
  if [[ ! -f ${file} ]]; then
    echo -e "${ERR}${file} does not exist, exiting ...${NC}" >&2
    exit 1
  fi
}

check_hosts_files() {
  echo -e "${INFO}Checking hosts files...${NC}"
  check_file_existence "${VISOR_HOSTS}"
  check_file_existence "${GRAPHER_HOSTS}"
  check_file_existence "${KEEPER_HOSTS}"
  check_file_existence "${PLAYER_HOSTS}"

  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Check hosts files done${NC}"
}

check_bin_files() {
  echo -e "${INFO}Checking binary files...${NC}"
  check_file_existence "${VISOR_BIN}"
  check_file_existence "${GRAPHER_BIN}"
  check_file_existence "${KEEPER_BIN}"
  check_file_existence "${PLAYER_BIN}"

  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Check binary files done${NC}"
}

check_rpc_comm_conf() {
  echo -e "${INFO}Checking if rpc conf matches on both communication ends ...${NC}"
  check_file_existence "${CONF_FILE}"
  check_file_existence "${CLIENT_CONF_FILE}"

  # for VisorClientPortalService, Client->Visor
  visor_client_portal_rpc_in_visor=$(jq '.chrono_visor.VisorClientPortalService.rpc' "${CONF_FILE}")
  visor_client_portal_rpc_in_client=$(jq '.chrono_client.VisorClientPortalService.rpc' "${CLIENT_CONF_FILE}")
  [[ "${visor_client_portal_rpc_in_visor}" != "${visor_client_portal_rpc_in_client}" ]] && echo -e "${ERR}mismatched VisorClientPortalService conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1

  # for VisorKeeperRegistryService, Keeper->Visor
  visor_keeper_registry_rpc_in_visor=$(jq '.chrono_visor.VisorKeeperRegistryService.rpc' "${CONF_FILE}")
  visor_keeper_registry_rpc_in_keeper=$(jq '.chrono_keeper.VisorKeeperRegistryService.rpc' "${CONF_FILE}")
  [[ "${visor_keeper_registry_rpc_in_visor}" != "${visor_keeper_registry_rpc_in_keeper}" ]] && echo -e "${ERR}mismatched VisorKeeperRegistryService conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1

  # for VisorGrapherRegistryService, Grapher->Visor
  visor_grapher_registry_rpc_in_grapher=$(jq '.chrono_grapher.VisorRegistryService.rpc' "${CONF_FILE}")
  [[ "${visor_keeper_registry_rpc_in_visor}" != "${visor_grapher_registry_rpc_in_grapher}" ]] && echo -e "${ERR}mismatched VisorGrapherRegistryService conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1

  # for VisorPlayerRegistryService, Player->Visor
  visor_player_registry_rpc_in_player=$(jq '.chrono_player.VisorRegistryService.rpc' "${CONF_FILE}")
  [[ "${visor_keeper_registry_rpc_in_visor}" != "${visor_player_registry_rpc_in_player}" ]] && echo -e "${ERR}mismatched VisorPlayerRegistryService conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1

  # for KeeperGrapherDrainService, Keeper->Grapher
  keeper_grapher_drain_rpc_in_keeper=$(jq '.chrono_keeper.KeeperGrapherDrainService.rpc' "${CONF_FILE}")
  keeper_grapher_drain_rpc_in_grapher=$(jq '.chrono_grapher.KeeperGrapherDrainService.rpc' "${CONF_FILE}")
  [[ "${keeper_grapher_drain_rpc_in_keeper}" != "${keeper_grapher_drain_rpc_in_grapher}" ]] && echo -e "${ERR}mismatched KeeperGrapherDrainService conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1

  # dataStoreAdminService protocol consistency
  keeper_data_store_admin_protocol=$(jq '.chrono_keeper.KeeperDataStoreAdminService.rpc.protocol_conf' "${CONF_FILE}")
  grapher_data_store_admin_protocol=$(jq '.chrono_grapher.DataStoreAdminService.rpc.protocol_conf' "${CONF_FILE}")
  player_data_store_admin_protocol=$(jq '.chrono_player.PlayerStoreAdminService.rpc.protocol_conf' "${CONF_FILE}")
  [[ "${keeper_data_store_admin_protocol}" != "${grapher_data_store_admin_protocol}" ]] && echo -e "${ERR}mismatched protocol for DataStoreAdminService in Keeper and Grapher conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1
  [[ "${keeper_data_store_admin_protocol}" != "${player_data_store_admin_protocol}" ]] && echo -e "${ERR}mismatched protocol for DataStoreAdminService in Keeper and Player conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1
  [[ "${grapher_data_store_admin_protocol}" != "${player_data_store_admin_protocol}" ]] && echo -e "${ERR}mismatched protocol for DataStoreAdminService in Grapher and Player conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1

  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Check rpc conf done${NC}"
}

check_op_validity() {
  count=0
  [[ $start == true ]] && ((count++))
  [[ $stop == true ]] && ((count++))
  [[ $clean == true ]] && ((count++))

  if [[ $count -ne 1 ]]; then
    echo -e "${ERR}Error: Please select exactly one operation in start (-d), stop (-s), and clean (-c).${NC}" >&2
    usage
  fi
}

check_recording_group_mapping() {
  echo -e "${INFO}Checking recording group mapping ...${NC}"
  local num_keepers
  local num_recording_group

  touch "${KEEPER_HOSTS}"
  num_keepers=$(wc -l <"${KEEPER_HOSTS}")
  num_recording_group=${NUM_RECORDING_GROUP}
  if [[ "${num_recording_group}" -lt 1 ]] || [[ "${num_recording_group}" -gt "${num_keepers}" ]]; then
    echo -e "${ERR}NUM_RECORDING_GROUP must be greater than 0, less than or equal to the number of keepers (${num_keepers}), exiting ...${NC}" >&2
    exit 1
  fi

  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Check recording group mapping done${NC}"
}

extract_shared_libraries() {
  local executable="$1"
  ldd_output=$(ldd ${executable} 2>/dev/null | grep '=>' | awk '{print $3}' | grep -v 'not' | grep -v '^/lib')
  echo "${ldd_output}"
}

copy_shared_libs_recursive() {
  local lib_path="$1"
  local dest_path="$2"
  local linked_to_lib_path

  linked_to_lib_path="$(readlink -f ${lib_path})"
  final_dest_lib_copies=false
  echo -e "${DEBUG}Copying ${lib_path} recursively ...${NC}"
  while [ "$final_dest_lib_copies" != true ]; do
    cp -P "$lib_path" "$dest_path/"
    if [ "$lib_path" == "$linked_to_lib_path" ]; then
      final_dest_lib_copies=true
    fi
    lib_path="$linked_to_lib_path"
    linked_to_lib_path="$(readlink -f ${lib_path})"
  done
}

copy_shared_libs() {
  echo -e "${DEBUG}Copying shared libraries ...${NC}"
  mkdir -p ${LIB_DIR}

  all_shared_libs=""
  for bin_file in "${WORK_DIR}"/bin/*; do
    if ${verbose}; then echo -e "${DEBUG}Extracting shared libraries from ${bin_file} ...${NC}"; fi
    all_shared_libs=$(echo -e "${all_shared_libs}\n$(extract_shared_libraries ${bin_file})" | sort | uniq)
  done

  for lib in ${all_shared_libs}; do
    if [[ -n ${lib} ]]; then
      copy_shared_libs_recursive ${lib} ${LIB_DIR}
    fi
  done

  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Copy shared libraries done${NC}"
}

detect_hs_net_suffix() {
  if [[ $(hostname) == *ares* ]]; then
    HOSTNAME_HS_NET_SUFFIX="-40g"
  fi
}

get_host_ip() {
  local hostname=$1
  local host_ip=""
  if [[ ${hostname} == *${HOSTNAME_HS_NET_SUFFIX} ]]; then
    host_ip=$(dig -4 ${hostname} | grep "^${hostname}" | awk '{print $5}')
  else
    host_ip=$(dig -4 ${hostname}${HOSTNAME_HS_NET_SUFFIX} | grep "^${hostname}${HOSTNAME_HS_NET_SUFFIX}" | awk '{print $5}')
  fi
  echo "${host_ip}"
}

get_remote_hostname() {
  local hostname=$1
  local remote_hostname=""
  remote_hostname=$(ssh -n ${hostname} hostname)
  echo "${remote_hostname}"
}

update_visor_ip() {
  visor_host=$(head -1 ${VISOR_HOSTS})
  visor_ip=$(get_host_ip ${visor_host})
  if [[ -z "${visor_ip}" ]]; then
    echo -e "${ERR}Cannot get ChronoVisor IP from hostname ${visor_host}, exiting ...${NC}" >&2
    exit 1
  fi

  echo -e "${INFO}Replacing ChronoVisor IP with ${visor_ip} ...${NC}"
  # Config File
  jq ".chrono_visor.VisorClientPortalService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} >tmp.json && mv tmp.json ${CONF_FILE}
  jq ".chrono_visor.VisorKeeperRegistryService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} >tmp.json && mv tmp.json ${CONF_FILE}
  jq ".chrono_keeper.VisorKeeperRegistryService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} >tmp.json && mv tmp.json ${CONF_FILE}
  jq ".chrono_grapher.VisorRegistryService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} >tmp.json && mv tmp.json ${CONF_FILE}
  jq ".chrono_player.VisorRegistryService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} >tmp.json && mv tmp.json ${CONF_FILE}

  # Client Config File
  jq ".chrono_client.VisorClientPortalService.rpc.service_ip = \"${visor_ip}\"" "${CLIENT_CONF_FILE}" >tmp.json && mv tmp.json "${CLIENT_CONF_FILE}"

  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Update ChronoVisor IP done${NC}"
}

update_visor_monitor_file_path() {
  visor_host=$(head -1 ${VISOR_HOSTS})
  jq ".chrono_visor.Monitoring.monitor.file = \"${MONITOR_DIR}/${VISOR_BIN_FILE_NAME}.${visor_host}.log\"" "${CONF_FILE}" >tmp.json && mv tmp.json "${CONF_FILE}"
}

update_client_monitor_file_path() {
  jq ".chrono_client.Monitoring.monitor.file = \"${MONITOR_DIR}/chrono_client.log\"" "${CLIENT_CONF_FILE}" >tmp.json && mv tmp.json "${CLIENT_CONF_FILE}"
}

generate_conf_for_each_keeper() {
  local base_conf_file=$1
  local keeper_hosts_file=$2
  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generating conf files for all ChronoKeepers in ${keeper_hosts_file} based on conf file ${base_conf_file} ...${NC}"
  while IFS= read -r keeper_host; do
    remote_keeper_hostname=$(get_remote_hostname ${keeper_host})
    [[ -z "${remote_keeper_hostname}" ]] && echo -e "${ERR}Cannot get hostname from ${keeper_host}, exiting ...${NC}" >&2 && exit 1
    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generating conf file ${base_conf_file}.keeper.${remote_keeper_hostname} for ChronoKeeper ${remote_keeper_hostname} ...${NC}"
    jq ".chrono_keeper.Monitoring.monitor.file = \"${MONITOR_DIR}/chrono_keeper.${remote_keeper_hostname}.log\"" "${base_conf_file}" >"${base_conf_file}.keeper.${remote_keeper_hostname}"
    keeper_ip=$(get_host_ip ${keeper_host})
    jq ".chrono_keeper.KeeperRecordingService.rpc.service_ip = \"${keeper_ip}\"" "${base_conf_file}.keeper.${remote_keeper_hostname}" >${CONF_DIR}/temp.json && mv ${CONF_DIR}/temp.json "${base_conf_file}.keeper.${remote_keeper_hostname}"
    jq ".chrono_keeper.KeeperDataStoreAdminService.rpc.service_ip = \"${keeper_ip}\"" "${base_conf_file}.keeper.${remote_keeper_hostname}" >${CONF_DIR}/temp.json && mv ${CONF_DIR}/temp.json "${base_conf_file}.keeper.${remote_keeper_hostname}"
  done <${keeper_hosts_file}

  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generate conf file for ChronoKeepers in ${keeper_hosts_file} done${NC}"
}

generate_conf_for_each_grapher() {
  local base_conf_file=$1
  local grapher_hosts_file=$2
  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generating conf files for all ChronoGraphers in ${grapher_hosts_file} based on conf file ${base_conf_file} ...${NC}"
  while IFS= read -r grapher_host; do
    remote_grapher_hostname=$(get_remote_hostname ${grapher_host})
    [[ -z "${remote_grapher_hostname}" ]] && echo -e "${ERR}Cannot get hostname from ${grapher_host}, exiting ...${NC}" >&2 && exit 1
    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generating conf file ${base_conf_file}.grapher.${remote_grapher_hostname} for ChronoGrapher ${remote_grapher_hostname} ...${NC}"
    jq ".chrono_grapher.Monitoring.monitor.file = \"${MONITOR_DIR}/chrono_grapher.${remote_grapher_hostname}.log\"" "${base_conf_file}" >"${base_conf_file}.grapher.${remote_grapher_hostname}"
  done <${grapher_hosts_file}

  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generate conf file for ChronoGraphers in ${grapher_hosts_file} done${NC}"
}

generate_conf_for_each_player() {
  local base_conf_file=$1
  local player_hosts_file=$2
  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generating conf files for all ChronoPlayers in ${player_hosts_file} based on conf file ${base_conf_file} ...${NC}"
  while IFS= read -r player_host; do
    remote_player_hostname=$(get_remote_hostname ${player_host})
    [[ -z "${remote_player_hostname}" ]] && echo -e "${ERR}Cannot get hostname from ${player_host}, exiting ...${NC}" >&2 && exit 1
    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generating conf file ${base_conf_file}.player.${remote_player_hostname} for ChronoPlayer ${remote_player_hostname} ...${NC}"
    jq ".chrono_player.Monitoring.monitor.file = \"${MONITOR_DIR}/chrono_player.${remote_player_hostname}.log\"" "${base_conf_file}" >"${base_conf_file}.player.${remote_player_hostname}"
  done <${player_hosts_file}

  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generate conf file for ChronoPlayers in ${player_hosts_file} done${NC}"
}

generate_conf_for_each_recording_group() {
  echo -e "${INFO}Generating conf files for all RecordingGroups ...${NC}"
  mkdir -p ${OUTPUT_DIR}
  for i in $(seq 1 ${NUM_RECORDING_GROUP}); do
    grapher_hosts_file="${GRAPHER_HOSTS}.${i}"
    keeper_hosts_file="${KEEPER_HOSTS}.${i}"
    player_hosts_file="${PLAYER_HOSTS}.${i}"
    num_graphers_in_hosts_file=$(wc -l <${grapher_hosts_file})
    [[ ${num_graphers_in_hosts_file} -ne 1 ]] && echo -e "${ERR}Exactly one node in ${grapher_hosts_file} is expected, exiting ...${NC}" >&2 && exit 1
    num_players_in_hosts_file=$(wc -l <${player_hosts_file})
    [[ ${num_players_in_hosts_file} -ne 1 ]] && echo -e "${ERR}Exactly one node in ${player_hosts_file} is expected, exiting ...${NC}" >&2 && exit 1

    grapher_hostname=$(head -1 ${grapher_hosts_file})
    grapher_ip=$(get_host_ip ${grapher_hostname})
    player_hostname=$(head -1 ${player_hosts_file})
    player_ip=$(get_host_ip ${player_hostname})

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Updating conf file for RecordingGroup ${i} ...${NC}"
    jq ".chrono_grapher.DataStoreAdminService.rpc.service_ip = \"${grapher_ip}\"" "${CONF_FILE}" >"${CONF_FILE}.${i}"
    jq ".chrono_keeper.RecordingGroup = ${i}" "${CONF_FILE}.${i}" >${CONF_DIR}/temp.json && mv ${CONF_DIR}/temp.json "${CONF_FILE}.${i}"
    jq ".chrono_grapher.RecordingGroup = ${i}" "${CONF_FILE}.${i}" >${CONF_DIR}/temp.json && mv ${CONF_DIR}/temp.json "${CONF_FILE}.${i}"
    jq ".chrono_player.RecordingGroup = ${i}" "${CONF_FILE}.${i}" >${CONF_DIR}/temp.json && mv ${CONF_DIR}/temp.json "${CONF_FILE}.${i}"
    jq ".chrono_keeper.KeeperGrapherDrainService.rpc.service_ip = \"${grapher_ip}\"" "${CONF_FILE}.${i}" >${CONF_DIR}/temp.json && mv ${CONF_DIR}/temp.json "${CONF_FILE}.${i}"
    jq ".chrono_grapher.KeeperGrapherDrainService.rpc.service_ip = \"${grapher_ip}\"" "${CONF_FILE}.${i}" >${CONF_DIR}/temp.json && mv ${CONF_DIR}/temp.json "${CONF_FILE}.${i}"
    jq ".chrono_keeper.Extractors.story_files_dir = \"${OUTPUT_DIR}\"" "${CONF_FILE}.${i}" >${CONF_DIR}/temp.json && mv ${CONF_DIR}/temp.json "${CONF_FILE}.${i}"
    jq ".chrono_grapher.Extractors.story_files_dir = \"${OUTPUT_DIR}\"" "${CONF_FILE}.${i}" >${CONF_DIR}/temp.json && mv ${CONF_DIR}/temp.json "${CONF_FILE}.${i}"
    jq ".chrono_player.PlayerStoreAdminService.rpc.service_ip = \"${player_ip}\"" "${CONF_FILE}.${i}" >${CONF_DIR}/temp.json && mv ${CONF_DIR}/temp.json "${CONF_FILE}.${i}"
    jq ".chrono_player.PlaybackQueryService.rpc.service_ip = \"${player_ip}\"" "${CONF_FILE}.${i}" >${CONF_DIR}/temp.json && mv ${CONF_DIR}/temp.json "${CONF_FILE}.${i}"
    jq ".chrono_player.ArchiveReaders.story_files_dir = \"${OUTPUT_DIR}\"" "${CONF_FILE}.${i}" >${CONF_DIR}/temp.json && mv ${CONF_DIR}/temp.json "${CONF_FILE}.${i}"

    generate_conf_for_each_keeper "${CONF_FILE}.${i}" "${keeper_hosts_file}"
    generate_conf_for_each_grapher "${CONF_FILE}.${i}" "${grapher_hosts_file}"
    generate_conf_for_each_player "${CONF_FILE}.${i}" "${player_hosts_file}"
  done
  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generate conf files for all RecordingGroups done${NC}"
}

prepare_hosts_for_recording_groups() {
  num_keepers_processed=0
  total_num_keepers=$(wc -l <${KEEPER_HOSTS})
  min_num_keepers_in_group=$((total_num_keepers / NUM_RECORDING_GROUP))
  missing_num_keepers_for_equal_group_size=$((NUM_RECORDING_GROUP - total_num_keepers + min_num_keepers_in_group * NUM_RECORDING_GROUP))
  if [[ $missing_num_keepers_for_equal_group_size -eq 0 ]]; then
    num_groups_with_extra_keeper=0
  else
    num_groups_with_extra_keeper=$((NUM_RECORDING_GROUP - missing_num_keepers_for_equal_group_size))
  fi
  for i in $(seq 1 ${NUM_RECORDING_GROUP}); do
    echo -e "${DEBUG}Generating hosts file for RecordingGroup ${i}...${NC}"
    num_keepers_in_this_group=0
    if [[ ${i} -le ${num_groups_with_extra_keeper} ]]; then
      num_keepers_in_this_group=$((min_num_keepers_in_group + 1))
    else
      num_keepers_in_this_group=${min_num_keepers_in_group}
    fi
    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Number of ChronoKeepers in RecordingGroup ${i}: ${num_keepers_in_this_group}${NC}"
    start_line_num_in_keeper_hosts=$((num_keepers_processed + 1))
    end_line_num_in_keeper_hosts=$((start_line_num_in_keeper_hosts + num_keepers_in_this_group - 1))
    sed -n "${start_line_num_in_keeper_hosts},${end_line_num_in_keeper_hosts}p" <${KEEPER_HOSTS} >${KEEPER_HOSTS}.${i}
    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}ChronoKeeper hosts in RecordingGroup ${i}: ${NC}" && cat ${KEEPER_HOSTS}.${i}
    sed -n "${i}p" <${GRAPHER_HOSTS} >${GRAPHER_HOSTS}.${i}
    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}ChronoGrapher host in RecordingGroup ${i}: ${NC}" && cat ${GRAPHER_HOSTS}.${i}
    sed -n "${i}p" <${PLAYER_HOSTS} >${PLAYER_HOSTS}.${i}
    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}ChronoPlayer host in RecordingGroup ${i}: ${NC}" && cat ${PLAYER_HOSTS}.${i}
    num_keepers_processed=$((end_line_num_in_keeper_hosts))
  done
}

prepare_hosts() {
  echo -e "${INFO}Preparing hosts files ...${NC}"

  hosts=""
  if [ -n "$SLURM_STEP_ID" ]; then
    echo -e "${ERR}ChronoLog does not support being launched using sbatch/srun, please create an interactive job and launch from a shell${NC}" >&2
    exit 1
  else
    echo -e "${DEBUG}Launched from a shell, getting hosts from command line or presets ...${NC}"
    if [ -n "${JOB_ID}" ]; then
      echo -e "${INFO}JOB_ID is set to be ${JOB_ID} via command line, use it${NC}"
      echo -e "${DEBUG}Node list from job ${JOB_ID} will overwrite what's provided in all hosts files${NC}"
      hosts_regex="$(squeue | grep ${JOB_ID} | awk '{print $NF}')"
      if [[ -z ${hosts_regex} ]]; then
        echo -e "${ERR}Cannot find job ${JOB_ID}, exiting ...${NC}" >&2
        exit 1
      fi
      hosts="$(scontrol show hostnames ${hosts_regex})"
    else
      echo -e "${DEBUG}JOB_ID is not set, use what are already in hosts files from ${CONF_DIR}${NC}"
      check_file_existence ${VISOR_HOSTS}
      check_file_existence ${GRAPHER_HOSTS}
      check_file_existence ${KEEPER_HOSTS}
      check_file_existence ${PLAYER_HOSTS}
    fi
  fi

  if [[ -n "${hosts}" ]]; then
    num_hosts=$(echo "${hosts}" | wc -l)
    if [[ ${NUM_RECORDING_GROUP} -gt "${num_hosts}" ]]; then
      echo -e "${ERR}There is no enough hosts for ${NUM_RECORDING_GROUP} RecordingGroups, only ${num_hosts} nodes are allocated, exiting ..${NC}"
      exit 1
    fi
    echo "${hosts}" | head -1 >${VISOR_HOSTS}
    echo "${hosts}" | tail -${NUM_RECORDING_GROUP} >${GRAPHER_HOSTS}
    echo "${hosts}" >${KEEPER_HOSTS}
    echo "${hosts}" | tail -${NUM_RECORDING_GROUP} >${PLAYER_HOSTS}
  fi

  check_recording_group_mapping
  prepare_hosts_for_recording_groups

  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Prepare hosts file done${NC}"
}

check_work_dir() {
  if [[ -z "${WORK_DIR}" ]]; then
    WORK_DIR="$(realpath -m "${SCRIPT_DIR}/..")"
    echo -e "${DEBUG}Using default work directory: ${WORK_DIR}${NC}"
  fi
}

parallel_remote_launch_processes() {
  local bin_dir=$1
  local lib_dir=$2
  local hosts_file=$3
  local bin_filename=$4
  local args="$5"
  local hostname_suffix=""
  local simple_output_grep_keyword=""

  hostname_suffix=".\$(hostname)"
  if [[ "${verbose}" == "false" ]]; then
    simple_output_grep_keyword="ares-"
  fi

  bin_path=${bin_dir}/${bin_filename}
  parallel-ssh -h ${hosts_file} -i "cd ${bin_dir}; nohup ${bin_path} ${args} > ${MONITOR_DIR}/${bin_filename}${hostname_suffix}.launch.log 2>&1 &" | grep "${simple_output_grep_keyword}" 2>&1 || true
}

parallel_remote_stop_processes() {
  local hosts_file=$1
  local bin_path=$2

  local bin_name
  bin_name=$(basename "${bin_path}")
  local timer=0
  parallel-ssh -h ${hosts_file} -i "pkill --signal 15 -ef ${bin_path}" 2>/dev/null
  while [[ -n $(parallel_remote_check_processes ${hosts_file} ${bin_path}) ]]; do
    echo -e "${DEBUG}${bin_name} processes are still running, waiting for 10 seconds ...${NC}"
    sleep 10
    timer=$((timer + 10))
    if [[ ${timer} -gt 300 ]]; then
      echo -e "${ERR}Killing ${bin_name} processes after 5 minutes ...${NC}" >&2
      parallel_remote_kill_processes ${hosts_file} ${bin_path}
      echo -e "${ERR}${bin_name} processes are killed${NC}" >&2
    fi
  done
}

parallel_remote_kill_processes() {
  local hosts_file=$1
  local bin_path=$2

  parallel-ssh -h ${hosts_file} -i "pkill --signal 9 -ef ${bin_path}" 2>/dev/null
}

parallel_remote_check_processes() {
  local hosts_file=$1
  local bin_path=$2

  parallel-ssh -h ${hosts_file} -i "pgrep -fla ${bin_path}" 2>/dev/null | sed '/^$/d' | grep -vE '^\[|ssh' || true
}

parallel_remote_check_all() {
  echo -e "${DEBUG}Running ChronoVisor:${NC}"
  parallel_remote_check_processes ${VISOR_HOSTS} ${VISOR_BIN}

  echo -e "${DEBUG}Running ChronoGraphers, ChronoKeepers, and ChronoPlayers:${NC}"
  for i in $(seq 1 ${NUM_RECORDING_GROUP}); do
    echo -e "${DEBUG}RecordingGroup ${i}:${NC}"
    grapher_hosts_file="${GRAPHER_HOSTS}.${i}"
    keeper_hosts_file="${KEEPER_HOSTS}.${i}"
    player_hosts_file="${PLAYER_HOSTS}.${i}"
    parallel_remote_check_processes ${grapher_hosts_file} ${GRAPHER_BIN}
    parallel_remote_check_processes ${keeper_hosts_file} ${KEEPER_BIN}
    parallel_remote_check_processes ${player_hosts_file} ${PLAYER_BIN}
  done
}

start() {
  check_work_dir

  echo -e "${INFO}Starting ...${NC}"
  mkdir -p "${MONITOR_DIR}" "${OUTPUT_DIR}"

  prepare_hosts
  check_bin_files

  update_visor_ip
  update_visor_monitor_file_path
  update_client_monitor_file_path
  generate_conf_for_each_recording_group
  check_rpc_comm_conf

  hostname_suffix=".\$(hostname)"
  if [[ "${verbose}" == "false" ]]; then
    simple_output_grep_keyword="ares-"
  fi

  echo -e "${DEBUG}Launching ChronoVisor ...${NC}"
  VISOR_BIN="${VISOR_BIN_DIR}/${VISOR_BIN_FILE_NAME}"
  VISOR_ARGS="--config ${CONF_FILE}"
  parallel_remote_launch_processes ${VISOR_BIN_DIR} ${LIB_DIR} ${VISOR_HOSTS} ${VISOR_BIN_FILE_NAME} "${VISOR_ARGS}"

  for i in $(seq 1 ${NUM_RECORDING_GROUP}); do
    local grapher_conf_file="${CONF_FILE}.${i}.grapher${hostname_suffix}"
    GRAPHER_BIN="${GRAPHER_BIN_DIR}/${GRAPHER_BIN_FILE_NAME}"
    GRAPHER_ARGS="--config ${grapher_conf_file}"
    grapher_hosts_file="${GRAPHER_HOSTS}.${i}"
    echo -e "${DEBUG}Launching ChronoGraphers from ${grapher_hosts_file} using conf file ${grapher_conf_file} ...${NC}"
    parallel_remote_launch_processes ${GRAPHER_BIN_DIR} ${LIB_DIR} ${grapher_hosts_file} ${GRAPHER_BIN_FILE_NAME} "${GRAPHER_ARGS}"

    local keeper_conf_file="${CONF_FILE}.${i}.keeper${hostname_suffix}"
    KEEPER_BIN="${KEEPER_BIN_DIR}/${KEEPER_BIN_FILE_NAME}"
    KEEPER_ARGS="--config ${keeper_conf_file}"
    keeper_hosts_file="${KEEPER_HOSTS}.${i}"
    echo -e "${DEBUG}Launching ChronoKeepers from ${keeper_hosts_file} using conf file ${keeper_conf_file} ...${NC}"
    parallel_remote_launch_processes ${KEEPER_BIN_DIR} ${LIB_DIR} ${keeper_hosts_file} ${KEEPER_BIN_FILE_NAME} "${KEEPER_ARGS}" &

    local player_conf_file="${CONF_FILE}.${i}.player${hostname_suffix}"
    PLAYER_BIN="${PLAYER_BIN_DIR}/${PLAYER_BIN_FILE_NAME}"
    PLAYER_ARGS="--config ${player_conf_file}"
    player_hosts_file="${PLAYER_HOSTS}.${i}"
    echo -e "${DEBUG}Launching ChronoPlayers from ${player_hosts_file} using conf file ${player_conf_file} ...${NC}"
    parallel_remote_launch_processes ${PLAYER_BIN_DIR} ${LIB_DIR} ${player_hosts_file} ${PLAYER_BIN_FILE_NAME} "${PLAYER_ARGS}" &

    wait
  done

  parallel_remote_check_all
  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Start done${NC}"
}

stop() {
  check_work_dir

  echo -e "${INFO}Stopping ...${NC}"

  if [[ -z ${JOB_ID} ]]; then
    echo -e "${DEBUG}No JOB_ID provided, use hosts files in ${CONF_DIR}${NC}"
    check_hosts_files
  else
    echo -e "${DEBUG}JOB_ID is provided, prepare hosts file first${NC}"
    prepare_hosts
  fi

  echo -e "${DEBUG}Stopping ChronoPlayer ...${NC}"
  parallel_remote_stop_processes ${PLAYER_HOSTS} ${PLAYER_BIN} &

  echo -e "${DEBUG}Stopping ChronoKeeper ...${NC}"
  parallel_remote_stop_processes ${KEEPER_HOSTS} ${KEEPER_BIN} &

  wait

  echo -e "${DEBUG}Stopping ChronoGrapher ...${NC}"
  parallel_remote_stop_processes ${GRAPHER_HOSTS} ${GRAPHER_BIN}

  wait

  echo -e "${DEBUG}Stopping ChronoVisor ...${NC}"
  parallel_remote_stop_processes ${VISOR_HOSTS} ${VISOR_BIN}

  parallel_remote_check_all

  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Stop done${NC}"
}

clean() {
  echo -e "${INFO}Cleaning ...${NC}"

  echo -e "${DEBUG}Checking if ChronoVisor is still running${NC}"
  [[ -n $(parallel_remote_check_processes ${VISOR_HOSTS} ${VISOR_BIN}) ]] && echo -e "${ERR}ChronoVisor is still running, please use stop (-s) to stop it first, exiting ...${NC}" >&2 && exit 1

  echo -e "${DEBUG}Checking if ChronoGraphers, ChronoKeepers, and ChronoPlayers are still running${NC}"
  for i in $(seq 1 ${NUM_RECORDING_GROUP}); do
    echo -e "${DEBUG}Checking RecordingGroup ${i}:${NC}"
    grapher_hosts_file="${GRAPHER_HOSTS}.${i}"
    keeper_hosts_file="${KEEPER_HOSTS}.${i}"
    player_hosts_file="${PLAYER_HOSTS}.${i}"
    [[ -n $(parallel_remote_check_processes ${grapher_hosts_file} ${GRAPHER_BIN}) ]] && echo -e "${ERR}ChronoGrapher is still running, please use stop (-s) to stop it first, exiting ...${NC}" >&2 && exit 1
    [[ -n $(parallel_remote_check_processes ${keeper_hosts_file} ${KEEPER_BIN}) ]] && echo -e "${ERR}ChronoKeeper is still running, please use stop (-s) to stop it first, exiting ...${NC}" >&2 && exit 1
    [[ -n $(parallel_remote_check_processes ${player_hosts_file} ${PLAYER_BIN}) ]] && echo -e "${ERR}ChronoPlayer is still running, please use stop (-s) to stop it first, exiting ...${NC}" >&2 && exit 1
  done

  echo -e "${DEBUG}Removing conf and hosts files ...${NC}"
  rm -f ${KEEPER_HOSTS}*.*
  rm -f ${GRAPHER_HOSTS}*.*
  rm -f ${PLAYER_HOSTS}*.*
  rm -f ${KEEPER_BIN}.*
  rm -f ${GRAPHER_BIN}.*
  rm -f ${PLAYER_BIN}.*
  rm -f ${MONITOR_DIR}/*.log

  echo -e "${DEBUG}Removing generated conf and output files ...${NC}"
  rm -f ${CONF_FILE}.*
  rm -f ${CLIENT_CONF_FILE}.*
  rm -f ${OUTPUT_DIR}/*

  [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Clean done${NC}"
}

parse_args() {
  TEMP=$(getopt -o w:m:u:v:g:p:a:q:k:o:f:n:j:r:hdsce --long work-dir:,monitor-dir:,output-dir:,visor-bin:,grapher-bin:,keeper-bin:,player-bin:,visor-hosts:,grapher-hosts:,keeper-hosts:,conf-file:,client-conf-file:,job-id:,record-groups:,help,start,stop,clean,verbose -- "$@")
  if [ $? != 0 ]; then
    echo -e "${ERR}Terminating ...${NC}" >&2
    exit 1
  fi

  eval set -- "$TEMP"
  while [[ $# -gt 0 ]]; do
    case "$1" in
    -w | --work-dir)
      mkdir -p "$2"
      WORK_DIR=$(realpath -m "${2%/}")
      LIB_DIR="${WORK_DIR}/lib"
      CONF_DIR="${WORK_DIR}/conf"
      BIN_DIR="${WORK_DIR}/bin"
      MONITOR_DIR="${WORK_DIR}/monitor"
      OUTPUT_DIR="${WORK_DIR}/output"
      VISOR_BIN_FILE_NAME="chronovisor_server"
      KEEPER_BIN_FILE_NAME="chrono_keeper"
      GRAPHER_BIN_FILE_NAME="chrono_grapher"
      VISOR_BIN="${WORK_DIR}/bin/${VISOR_BIN_FILE_NAME}"
      GRAPHER_BIN="${WORK_DIR}/bin/${GRAPHER_BIN_FILE_NAME}"
      KEEPER_BIN="${WORK_DIR}/bin/${KEEPER_BIN_FILE_NAME}"
      PLAYER_BIN="${WORK_DIR}/bin/${PLAYER_BIN_FILE_NAME}"
      VISOR_BIN_DIR=$(dirname ${VISOR_BIN})
      GRAPHER_BIN_DIR=$(dirname ${GRAPHER_BIN})
      KEEPER_BIN_DIR=$(dirname ${KEEPER_BIN})
      PLAYER_BIN_DIR=$(dirname ${PLAYER_BIN})
      CONF_FILE="${CONF_DIR}/default_conf.json"
      CLIENT_CONF_FILE="${CONF_DIR}/default_client_conf.json"
      VISOR_ARGS="--config ${CONF_FILE}"
      GRAPHER_ARGS="--config ${CONF_FILE}"
      KEEPER_ARGS="--config ${CONF_FILE}"
      PLAYER_ARGS="--config ${CONF_FILE}"
      VISOR_HOSTS="${CONF_DIR}/hosts_visor"
      GRAPHER_HOSTS="${CONF_DIR}/hosts_grapher"
      KEEPER_HOSTS="${CONF_DIR}/hosts_keeper"
      PLAYER_HOSTS="${CONF_DIR}/hosts_player"
      [ -s "${GRAPHER_HOSTS}" ] && NUM_RECORDING_GROUP=$(wc -l <"${GRAPHER_HOSTS}")
      mkdir -p ${MONITOR_DIR}
      mkdir -p ${OUTPUT_DIR}
      shift 2
      ;;
    -m | --monitor-dir)
      MONITOR_DIR=$(realpath -m "$2")
      shift 2
      ;;
    -u | --output-dir)
      OUTPUT_DIR=$(realpath -m "$2")
      mkdir -p ${OUTPUT_DIR}
      shift 2
      ;;
    -v | --visor-bin)
      VISOR_BIN=$(realpath -m "$2")
      VISOR_BIN_FILE_NAME=$(basename ${VISOR_BIN})
      VISOR_BIN_DIR=$(dirname ${VISOR_BIN})
      shift 2
      ;;
    -g | --grapher-bin)
      GRAPHER_BIN=$(realpath -m "$2")
      GRAPHER_BIN_FILE_NAME=$(basename ${GRAPHER_BIN})
      GRAPHER_BIN_DIR=$(dirname ${GRAPHER_BIN})
      shift 2
      ;;
    -p | --keeper-bin)
      KEEPER_BIN=$(realpath -m "$2")
      KEEPER_BIN_FILE_NAME=$(basename ${KEEPER_BIN})
      KEEPER_BIN_DIR=$(dirname ${KEEPER_BIN})
      shift 2
      ;;
    -a | --player-bin)
      PLAYER_BIN=$(realpath -m "$2")
      PLAYER_BIN_FILE_NAME=$(basename ${PLAYER_BIN})
      PLAYER_BIN_DIR=$(dirname ${PLAYER_BIN})
      shift 2
      ;;
    -q | --visor-hosts)
      VISOR_HOSTS=$(realpath -m "$2")
      shift 2
      ;;
    -k | --grapher-hosts)
      GRAPHER_HOSTS=$(realpath -m "$2")
      shift 2
      ;;
    -o | --keeper-hosts)
      KEEPER_HOSTS=$(realpath -m "$2")
      shift 2
      ;;
    -f | --conf-file)
      CONF_FILE=$(realpath -m "$2")
      CONF_DIR=$(dirname ${CONF_FILE})
      shift 2
      ;;
    -n | --client-conf-file)
      CLIENT_CONF_FILE=$(realpath -m "$2")
      shift 2
      ;;
    -j | --job-id)
      JOB_ID="$2"
      shift 2
      ;;
    -r | --record-groups)
      NUM_RECORDING_GROUP="$2"
      shift 2
      ;;
    -h | --help)
      usage
      shift 2
      ;;
    -d | --start)
      start=true
      shift
      ;;
    -s | --stop)
      stop=true
      shift
      ;;
    -c | --clean)
      clean=true
      shift
      ;;
    -e | --verbose)
      verbose=true
      shift
      ;;
    --)
      shift
      break
      ;;
    *)
      echo -e "${ERR}Unknown option: $1${NC}"
      exit 1
      ;;
    esac
  done

  shift $((OPTIND - 1))
}

# Parse arguments
parse_args "$@"

# Check dependencies
check_dependencies

# Ares-specific settings
detect_hs_net_suffix

# Check requested operation
check_op_validity

if ${start}; then
  start
elif ${stop}; then
  stop
elif ${clean}; then
  clean
fi

echo -e "${INFO}Done${NC}"
