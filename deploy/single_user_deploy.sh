#!/bin/bash
# shellcheck disable=SC2086

# Define some colors
ERR='\033[7;37m\033[41m'
INFO='\033[7;49m\033[92m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

BUILD_TYPE="Release"
INSTALL_DIR="/home/$USER/chronolog/${BUILD_TYPE}"
WORK_DIR=""
LIB_DIR=""
CONF_DIR=""
MONITOR_DIR=""
OUTPUT_DIR=""
VISOR_BIN_FILE_NAME="chronovisor_server"
GRAPHER_BIN_FILE_NAME="chrono_grapher"
KEEPER_BIN_FILE_NAME="chrono_keeper"
VISOR_BIN=""
GRAPHER_BIN=""
KEEPER_BIN=""
VISOR_BIN_DIR=""
GRAPHER_BIN_DIR=""
KEEPER_BIN_DIR=""
CONF_FILE=""
VISOR_ARGS="--config ${CONF_FILE}"
GRAPHER_ARGS="--config ${CONF_FILE}"
KEEPER_ARGS="--config ${CONF_FILE}"
VISOR_HOSTS=""
GRAPHER_HOSTS=""
KEEPER_HOSTS=""
NUM_RECORDING_GROUP=1
HOSTNAME_HS_NET_SUFFIX="-40g"
JOB_ID=""
build=false
install=false
start=false
stop=false
clean=false
verbose=false

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  -h|--help           Display this help and exit"
    echo ""
    echo "Execution Modes (Select only ONE):"
    echo "  -b|--build          Build ChronoLog (default: false)"
    echo "  -i|--install        Install ChronoLog (default: false)"
    echo "  -d|--start          Start ChronoLog Deployment (default: false)"
    echo "  -s|--stop           Stop ChronoLog Deployment (default: false)"
    echo "  -c|--clean          Clean ChronoLog logging artifacts and output of previous deployments (default: false)"
    echo "  Note: Only one execution mode can be selected per run."
    echo ""
    echo "Build Options:"
    echo "  -t|--build-type     Define type of build: Debug | Release (default: Release) [Modes: Build]"
    echo "  -l|--install-dir    Define installation directory (default: /home/$USER/chronolog/BUILD_TYPE) [Modes: Build]"
    echo ""
    echo "Deployment Options:"
    echo "  -r|--record-groups  Set the number of RecordingGroups or ChronoGrapher processes [Modes: Start]"
    echo "  -j|--job-id         JOB_ID Set the job ID to get hosts from (default: \"\") [Modes: Start]"
    echo "  -q|--visor-hosts    VISOR_HOSTS Set the hosts file for ChronoVisor (default: work_dir/conf/hosts_visor) [Modes: Start]"
    echo "  -a|--grapher-hosts  GRAPHER_HOSTS Set the hosts file for ChronoGrapher (default: work_dir/conf/hosts_grapher) [Modes: Start]"
    echo "  -o|--keeper-hosts   KEEPER_HOSTS Set the hosts file for ChronoKeeper (default: work_dir/conf/hosts_keeper) [Modes: Start]"
    echo ""
    echo "Directory Settings:"
    echo "  -w|--work-dir       WORK_DIR Set the working directory (Mandatory) [Modes: Install, Start, Stop, Clean]"
    echo "  -m|--monitor-dir    MONITOR_DIR (default: work_dir/monitor) [Modes: Start]"
    echo "  -u|--output-dir     OUTPUT_DIR (default: work_dir/output) [Modes: Start]"
    echo ""
    echo "Binary Paths:"
    echo "  -v|--visor-bin      VISOR_BIN (default: work_dir/bin/chronovisor_server) [Modes: Start]"
    echo "  -g|--grapher-bin    GRAPHER_BIN (default: work_dir/bin/chrono_grapher) [Modes: Start]"
    echo "  -p|--keeper-bin     KEEPER_BIN (default: work_dir/bin/chrono_keeper) [Modes: Start]"
    echo ""
    echo "Configuration Settings:"
    echo "  -f|--conf-file      CONF_FILE Path to the configuration file (default: work_dir/conf/default_conf.json) [Modes: Start]"
    echo ""
    echo "Miscellaneous Options:"
    echo "  -e|--verbose        Enable verbose output (default: false)"
    echo ""
    echo "Examples:"
    echo "  $0 --build --build-type Debug"
    echo "  $0 --start --work-dir /path/to/workdir --conf-file /path/to/config.json"
    echo ""
    exit 1
}

check_dependencies() {
    local dependencies=("jq" "mpssh" "ssh" "ldd" "nohup" "pkill" "readlink")

    echo -e "${DEBUG}Checking required dependencies...${NC}"
    for dep in "${dependencies[@]}"; do
        if ! command -v $dep &> /dev/null; then
            echo -e "${ERR}Dependency $dep is not installed. Please install it and try again.${NC}"
            exit 1
        fi
    done
    echo -e "${DEBUG}All required dependencies are installed.${NC}"
}

check_file_existence() {
    local file=$1
    if [[ ! -f ${file} ]]; then
        echo -e "${ERR}${file} host file does not exist, exiting ...${NC}" >&2
        exit 1
    fi
}

check_hosts_files() {
    echo -e "${INFO}Checking hosts files...${NC}"
    check_file_existence ${VISOR_HOSTS}
    check_file_existence ${GRAPHER_HOSTS}
    check_file_existence ${KEEPER_HOSTS}

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Check hosts files done${NC}"
}

check_bin_files() {
    echo -e "${INFO}Checking binary files...${NC}"
    check_file_existence ${VISOR_BIN}
    check_file_existence ${GRAPHER_BIN}
    check_file_existence ${KEEPER_BIN}

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Check binary files done${NC}"
}

check_rpc_comm_conf() {
    echo -e "${INFO}Checking if rpc conf matches on both communication ends ...${NC}"
    check_file_existence ${CONF_FILE}

    # for VisorClientPortalService, Client->Visor
    visor_client_portal_rpc_in_visor=$(jq '.chrono_visor.VisorClientPortalService.rpc' "${CONF_FILE}")
    visor_client_portal_rpc_in_client=$(jq '.chrono_client.VisorClientPortalService.rpc' "${CONF_FILE}")
    [[ "${visor_client_portal_rpc_in_visor}" != "${visor_client_portal_rpc_in_client}" ]] && echo -e "${ERR}mismatched VisorClientPortalService conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1

    # for VisorKeeperRegistryService, Keeper->Visor
    visor_keeper_registry_rpc_in_visor=$(jq '.chrono_visor.VisorKeeperRegistryService.rpc' "${CONF_FILE}")
    visor_keeper_registry_rpc_in_keeper=$(jq '.chrono_keeper.VisorKeeperRegistryService.rpc' "${CONF_FILE}")
    [[ "${visor_keeper_registry_rpc_in_visor}" != "${visor_keeper_registry_rpc_in_keeper}" ]] && echo -e "${ERR}mismatched VisorKeeperRegistryService conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1

    # for VisorRegistryService, Grapher->Visor
    visor_grapher_registry_rpc_in_grapher=$(jq '.chrono_grapher.VisorRegistryService.rpc' "${CONF_FILE}")
    [[ "${visor_keeper_registry_rpc_in_visor}" != "${visor_grapher_registry_rpc_in_grapher}" ]] && echo -e "${ERR}mismatched VisorGrapherRegistryService conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1

    # for KeeperGrapherDrainService, Keeper->Grapher
    keeper_grapher_drain_rpc_in_keeper=$(jq '.chrono_keeper.KeeperGrapherDrainService.rpc' "${CONF_FILE}")
    keeper_grapher_drain_rpc_in_grapher=$(jq '.chrono_grapher.KeeperGrapherDrainService.rpc' "${CONF_FILE}")
    [[ "${keeper_grapher_drain_rpc_in_keeper}" != "${keeper_grapher_drain_rpc_in_grapher}" ]] && echo -e "${ERR}mismatched KeeperGrapherDrainService conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1

    # to assure Keeper and Grapher use the same protocol for dataStoreAdminService
    keeper_data_store_admin_protocol=$(jq '.chrono_keeper.KeeperDataStoreAdminService.rpc.protocol_conf' "${CONF_FILE}")
    grapher_data_store_admin_protocol=$(jq '.chrono_grapher.DataStoreAdminService.rpc.protocol_conf' "${CONF_FILE}")
    [[ "${keeper_data_store_admin_protocol}" != "${grapher_data_store_admin_protocol}" ]] && echo -e "${ERR}mismatched protocol for DataStoreAdminService in Keeper and Grapher conf in ${CONF_FILE}, exiting ...${NC}" >&2 && exit 1

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Check rpc conf done${NC}"

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Check base conf file done${NC}"
}

check_op_validity() {
    count=0
    [[ $build == true ]] && ((count++))
    [[ $install == true ]] && ((count++))
    [[ $start == true ]] && ((count++))
    [[ $stop == true ]] && ((count++))
    [[ $clean == true ]] && ((count++))

    if [[ $count -ne 1 ]]; then
        echo -e "${ERR}Error: Please select exactly one operation in build (-b), install (-i), start (-d), stop (-s), and clean (-c).${NC}" >&2
        usage
    fi
}

check_recording_group_mapping() {
    echo -e "${INFO}Checking recording group mapping ...${NC}"
    local num_keepers
    local num_recording_group

    touch ${KEEPER_HOSTS}
    num_keepers=$(wc -l <${KEEPER_HOSTS})
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
    # Copy the library and maintain symbolic links recursively
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
    # Copy shared libraries to the lib directory
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

update_visor_ip() {
    visor_host=$(head -1 ${VISOR_HOSTS})
    visor_ip=$(get_host_ip ${visor_host})
    if [[ -z "${visor_ip}" ]]; then
        echo -e "${ERR}Cannot get ChronoVisor IP from hostname ${visor_host}, exiting ...${NC}" >&2
        exit 1
    fi
    echo -e "${INFO}Replacing ChronoVisor IP with ${visor_ip} ...${NC}"
    jq ".chrono_visor.VisorClientPortalService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} >tmp.json && mv tmp.json ${CONF_FILE}
    jq ".chrono_client.VisorClientPortalService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} >tmp.json && mv tmp.json ${CONF_FILE}
    jq ".chrono_visor.VisorKeeperRegistryService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} >tmp.json && mv tmp.json ${CONF_FILE}
    jq ".chrono_keeper.VisorKeeperRegistryService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} >tmp.json && mv tmp.json ${CONF_FILE}
    jq ".chrono_grapher.VisorRegistryService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} >tmp.json && mv tmp.json ${CONF_FILE}

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Update ChronoVisor IP done${NC}"
}

update_visor_client_monitor_file_path() {
    visor_host=$(head -1 ${VISOR_HOSTS})
    echo -e "${INFO}Updating monitoring file path ...${NC}"
    jq ".chrono_visor.Monitoring.monitor.file = \"${MONITOR_DIR}/${VISOR_BIN_FILE_NAME}.${visor_host}.log\"" ${CONF_FILE} >tmp.json && mv tmp.json ${CONF_FILE}
    jq ".chrono_client.Monitoring.monitor.file = \"${MONITOR_DIR}/chrono_client.log\"" ${CONF_FILE} >tmp.json && mv tmp.json ${CONF_FILE}
}

generate_conf_for_each_keeper() {
    local base_conf_file=$1
    local keeper_hosts_file=$2
    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generating conf files for all ChronoKeepers in ${keeper_hosts_file} based on conf file ${base_conf_file} ...${NC}"
    while IFS= read -r keeper_host; do
        remote_keeper_hostname=$(LD_LIBRARY_PATH=/lib/x86_64-linux-gnu/ ssh -n ${keeper_host} hostname)
        [[ -z "${remote_keeper_hostname}" ]] && echo -e "${ERR}Cannot get hostname from ${keeper_host}, exiting ...${NC}" >&2 && exit 1
        [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generating conf file ${base_conf_file}.keeper.${remote_keeper_hostname} for ChronoKeeper ${remote_keeper_hostname} ...${NC}"
        jq ".chrono_keeper.Monitoring.monitor.file = \"${MONITOR_DIR}/chrono_keeper.${remote_keeper_hostname}.log\"" "${base_conf_file}" >"${base_conf_file}.keeper.${remote_keeper_hostname}"
        keeper_ip=$(get_host_ip ${keeper_host})
        jq ".chrono_keeper.KeeperRecordingService.rpc.service_ip = \"${keeper_ip}\"" "${base_conf_file}.keeper.${remote_keeper_hostname}" >temp.json && mv temp.json "${base_conf_file}.keeper.${remote_keeper_hostname}"
        jq ".chrono_keeper.KeeperDataStoreAdminService.rpc.service_ip = \"${keeper_ip}\"" "${base_conf_file}.keeper.${remote_keeper_hostname}" >temp.json && mv temp.json "${base_conf_file}.keeper.${remote_keeper_hostname}"
    done <${keeper_hosts_file}

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generate conf file for ChronoKeepers in ${keeper_hosts_file} done${NC}"
}

generate_conf_for_each_grapher() {
    local base_conf_file=$1
    local grapher_hosts_file=$2
    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generating conf files for all ChronoGraphers in ${grapher_hosts_file} based on conf file ${base_conf_file} ...${NC}"
    while IFS= read -r grapher_host; do
        remote_grapher_hostname=$(LD_LIBRARY_PATH=/lib/x86_64-linux-gnu/ ssh -n ${grapher_host} hostname)
        [[ -z "${remote_grapher_hostname}" ]] && echo -e "${ERR}Cannot get hostname from ${grapher_host}, exiting ...${NC}" >&2 && exit 1
        [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generating conf file ${base_conf_file}.grapher.${remote_grapher_hostname} for ChronoGrapher ${remote_grapher_hostname} ...${NC}"
        jq ".chrono_grapher.Monitoring.monitor.file = \"${MONITOR_DIR}/chrono_grapher.${remote_grapher_hostname}.log\"" "${base_conf_file}" >"${base_conf_file}.grapher.${remote_grapher_hostname}"
        #grapher_ip=$(get_host_ip ${grapher_host})
        #jq ".chrono_keeper.KeeperGrapherDrainService.rpc.service_ip = \"${grapher_ip}\"" "${base_conf_file}".grapher.${remote_grapher_hostname}" > temp.json && mv temp.json "${base_conf_file}.grapher.${remote_grapher_hostname}"
        #jq ".chrono_grapher.KeeperGrapherDrainService.rpc.service_ip = \"${grapher_ip}\"" "${base_conf_file}.grapher.${remote_grapher_hostname}" > temp.json && mv temp.json "${base_conf_file}.grapher.${remote_grapher_hostname}"
    done <${grapher_hosts_file}

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generate conf file for ChronoGraphers in ${grapher_hosts_file} done${NC}"
}

generate_conf_for_each_recording_group() {
    echo -e "${INFO}Generating conf files for all RecordingGroups ...${NC}"
    mkdir -p ${OUTPUT_DIR}
    # grapher in recording group i will use conf file ${CONF_FILE}.${i}
    # keeper in recording group i will use conf file ${CONF_FILE}.${i}.${keeper_hostname}
    for i in $(seq 1 ${NUM_RECORDING_GROUP}); do
        grapher_hosts_file="${GRAPHER_HOSTS}.${i}"
        keeper_hosts_file="${KEEPER_HOSTS}.${i}"
        num_graphers_in_hosts_file=$(wc -l <${grapher_hosts_file})
        [[ ${num_graphers_in_hosts_file} -ne 1 ]] && echo -e "${ERR}Exactly one node in ${grapher_hosts_file} is expected, exiting ...${NC}" >&2 && exit 1
        grapher_hostname=$(head -1 ${grapher_hosts_file})
        grapher_ip=$(get_host_ip ${grapher_hostname})
        remote_grapher_hostname=$(LD_LIBRARY_PATH=/lib/x86_64-linux-gnu/ ssh -n ${grapher_hostname} hostname)
        [[ -z "${remote_grapher_hostname}" ]] && echo -e "${ERR}Cannot get hostname from ${grapher_hostname}, exiting ...${NC}" >&2 && exit 1
        [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Updating conf file for members in RecordingGroup ${i} ...${NC}"
        jq ".chrono_grapher.DataStoreAdminService.rpc.service_ip = \"${grapher_ip}\"" "${CONF_FILE}" >"${CONF_FILE}.${i}"
        jq ".chrono_keeper.RecordingGroup = ${i}" "${CONF_FILE}.${i}" >temp.json && mv temp.json "${CONF_FILE}.${i}"
        jq ".chrono_grapher.RecordingGroup = ${i}" "${CONF_FILE}.${i}" >temp.json && mv temp.json "${CONF_FILE}.${i}"
        jq ".chrono_keeper.KeeperGrapherDrainService.rpc.service_ip = \"${grapher_ip}\"" "${CONF_FILE}.${i}" >temp.json && mv temp.json "${CONF_FILE}.${i}"
        jq ".chrono_grapher.KeeperGrapherDrainService.rpc.service_ip = \"${grapher_ip}\"" "${CONF_FILE}.${i}" >temp.json && mv temp.json "${CONF_FILE}.${i}"
        jq ".chrono_keeper.story_files_dir = \"${OUTPUT_DIR}\"" "${CONF_FILE}.${i}" >temp.json && mv temp.json "${CONF_FILE}.${i}"
        jq ".chrono_grapher.Extractors.story_files_dir = \"${OUTPUT_DIR}\"" "${CONF_FILE}.${i}" >temp.json && mv temp.json "${CONF_FILE}.${i}"

        generate_conf_for_each_keeper "${CONF_FILE}.${i}" "${keeper_hosts_file}"

        generate_conf_for_each_grapher "${CONF_FILE}.${i}" "${grapher_hosts_file}"
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
        num_keepers_processed=$((end_line_num_in_keeper_hosts))
    done
}

prepare_hosts() {
    echo -e "${INFO}Preparing hosts files ...${NC}"

    hosts=""
    if [ -n "$SLURM_JOB_ID" ]; then
        echo -e "${DEBUG}Launched as a SLURM job, getting hosts from job ${SLURM_JOB_ID} ...${NC}"
        echo -e "${DEBUG}Node list from this job will overwrite what's provided in all hosts files${NC}"
        hosts="$(echo \"$SLURM_JOB_NODELIST\" | tr ',' '\n')"
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
        fi
    fi

    if [[ -n "${hosts}" ]]; then
        # make sure we have enough nodes allocated
        num_hosts=$(echo "${hosts}" | wc -l)
        if [[ ${NUM_RECORDING_GROUP} -gt "${num_hosts}" ]]; then
            echo -e "${ERR}There is no enough hosts for ${NUM_RECORDING_GROUP} RecordingGroups, only ${num_hosts} nodes are allocated, exiting ..${NC}"
            exit 1
        fi
        # use the first node as Visor by default
        echo "${hosts}" | head -1 >${VISOR_HOSTS}
        # use last num_recording_group nodes as Graphers by default
        echo "${hosts}" | tail -${NUM_RECORDING_GROUP} >${GRAPHER_HOSTS}
        # use all nodes as Keepers by default
        echo "${hosts}" >${KEEPER_HOSTS}
    fi

    check_recording_group_mapping

    prepare_hosts_for_recording_groups

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Prepare hosts file done${NC}"
}

build() {
    echo -e "${INFO}Building ...${NC}"

    # build ChronoLog
    if [[ -n "$INSTALL_DIR" ]]; then
        ./build.sh -type "$BUILD_TYPE" -install-path "$INSTALL_DIR"
    else
        ./build.sh -type "$BUILD_TYPE"
    fi

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Build done${NC}"
}

install() {
    echo -e "${INFO}Installing ...${NC}"

    check_dependencies

    copy_shared_libs

    prepare_hosts

    check_bin_files

    update_visor_ip

    update_visor_client_monitor_file_path

    generate_conf_for_each_recording_group

    check_rpc_comm_conf

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Install done${NC}"
}

parallel_remote_launch_processes() {
    local bin_dir=$1
    local lib_dir=$2
    local hosts_file=$3
    local bin_filename=$4
    local args="$5"
    local hostname_suffix=""
    local simple_output_grep_keyword=""

    # use hostname suffix conf file only on Ares
    hostname_suffix=".\$(hostname)"
    if [[ "${verbose}" == "false" ]]; then
        # grep only on Ares with simple output
        simple_output_grep_keyword="ares-"
    fi

    bin_path=${bin_dir}/${bin_filename}
    LD_LIBRARY_PATH=/lib/x86_64-linux-gnu/ mpssh -f ${hosts_file} "cd ${bin_dir}; LD_LIBRARY_PATH=${lib_dir} nohup ${bin_path} ${args} > ${MONITOR_DIR}/${bin_filename}${hostname_suffix}.launch.log 2>&1 &" | grep "${simple_output_grep_keyword}" 2>&1
}

parallel_remote_stop_processes() {
    local hosts_file=$1
    local bin_filename=$2

    local timer=0
    LD_LIBRARY_PATH=/lib/x86_64-linux-gnu/ mpssh -f ${hosts_file} "pkill --signal 15 -ef ${bin_filename}" | grep -e "->" | grep -v ssh 2>&1
    while [[ -n $(parallel_remote_check_processes ${hosts_file} ${bin_filename}) ]]; do
        echo -e "${DEBUG}Some processes are still running, waiting for 10 seconds ...${NC}"
        sleep 10
        timer=$((timer + 10))
        if [[ ${timer} -gt 300 ]]; then
            echo -e "${ERR}Killing processes after 5 minutes ...${NC}" >&2
            parallel_remote_kill_processes ${hosts_file} ${bin_filename}
            exit 1
        fi
    done
}

parallel_remote_kill_processes() {
    local hosts_file=$1
    local bin_filename=$2

    LD_LIBRARY_PATH=/lib/x86_64-linux-gnu/ mpssh -f ${hosts_file} "pkill --signal 9 -ef ${bin_filename}" | grep -e "->" | grep -v ssh 2>&1
}

parallel_remote_check_processes() {
    local hosts_file=$1
    local bin_filename=$2

    LD_LIBRARY_PATH=/lib/x86_64-linux-gnu/ mpssh -f ${hosts_file} "pgrep -fla ${bin_filename}" | grep -e "->" | grep -v ssh 2>&1
}

parallel_remote_check_all() {
    # check Visor
    if [[ "${install}" == "true" ]]; then
        echo -e "${DEBUG}Running ChronoVisor (only one is expected):${NC}"
    else
        echo -e "${DEBUG}Running ChronoVisor:${NC}"
    fi
    parallel_remote_check_processes ${VISOR_HOSTS} ${VISOR_BIN_FILE_NAME}

    # check Grapher and Keeper in group
    echo -e "${DEBUG}Running ChronoGraphers and ChronoKeepers:${NC}"
    for i in $(seq 1 ${NUM_RECORDING_GROUP}); do
        echo -e "${DEBUG}RecordingGroup ${i}:${NC}"
        grapher_hosts_file="${GRAPHER_HOSTS}.${i}"
        keeper_hosts_file="${KEEPER_HOSTS}.${i}"
        parallel_remote_check_processes ${grapher_hosts_file} ${GRAPHER_BIN_FILE_NAME}
        parallel_remote_check_processes ${keeper_hosts_file} ${KEEPER_BIN_FILE_NAME}
    done
}

start() {
    echo -e "${INFO}Starting ...${NC}"

    local hostname_suffix=""
    local simple_output_grep_keyword=""

    # use hostname suffix conf file only on Ares
    hostname_suffix=".\$(hostname)"
    if [[ "${verbose}" == "false" ]]; then
        # grep only on Ares with simple output
        simple_output_grep_keyword="ares-"
    fi

    # launch Visor
    echo -e "${DEBUG}Launching ChronoVisor ...${NC}"
    VISOR_BIN="${VISOR_BIN_DIR}/${VISOR_BIN_FILE_NAME}"
    VISOR_ARGS="--config ${CONF_FILE}"
    parallel_remote_launch_processes ${VISOR_BIN_DIR} ${LIB_DIR} ${VISOR_HOSTS} ${VISOR_BIN_FILE_NAME} "${VISOR_ARGS}"

    # launch Grapher and Keeper in group
    for i in $(seq 1 ${NUM_RECORDING_GROUP}); do
        # launch Grapher
        local grapher_conf_file="${CONF_FILE}.${i}.grapher${hostname_suffix}"
        GRAPHER_BIN="${GRAPHER_BIN_DIR}/${GRAPHER_BIN_FILE_NAME}"
        GRAPHER_ARGS="--config ${grapher_conf_file}"
        grapher_hosts_file="${GRAPHER_HOSTS}.${i}"
        echo -e "${DEBUG}Launching ChronoGraphers from ${grapher_hosts_file} using conf file ${grapher_conf_file} ...${NC}"
        parallel_remote_launch_processes ${GRAPHER_BIN_DIR} ${LIB_DIR} ${grapher_hosts_file} ${GRAPHER_BIN_FILE_NAME} "${GRAPHER_ARGS}"

        # launch Keeper
        local keeper_conf_file="${CONF_FILE}.${i}.keeper${hostname_suffix}"
        KEEPER_BIN="${KEEPER_BIN_DIR}/${KEEPER_BIN_FILE_NAME}"
        KEEPER_ARGS="--config ${keeper_conf_file}"
        keeper_hosts_file="${KEEPER_HOSTS}.${i}"
        echo -e "${DEBUG}Launching ChronoKeepers from ${keeper_hosts_file} using conf file ${keeper_conf_file} ...${NC}"
        parallel_remote_launch_processes ${KEEPER_BIN_DIR} ${LIB_DIR} ${keeper_hosts_file} ${KEEPER_BIN_FILE_NAME} "${KEEPER_ARGS}"
    done

    parallel_remote_check_all

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Start done${NC}"
}

stop() {
    echo -e "${INFO}Stopping ...${NC}"

    if [[ -z ${JOB_ID} ]]; then
        echo -e "${DEBUG}No JOB_ID provided, use hosts files in ${CONF_DIR}${NC}"
        check_hosts_files
    else
        echo -e "${DEBUG}JOB_ID is provided, prepare hosts file first${NC}"
        prepare_hosts
    fi

    # stop Keeper
    echo -e "${DEBUG}Stopping ChronoKeeper ...${NC}"
    parallel_remote_stop_processes ${KEEPER_HOSTS} ${KEEPER_BIN_FILE_NAME}

    # stop Grapher
    echo -e "${DEBUG}Stopping ChronoGrapher ...${NC}"
    parallel_remote_stop_processes ${GRAPHER_HOSTS} ${GRAPHER_BIN_FILE_NAME}

    # stop Visor
    echo -e "${DEBUG}Stopping ChronoVisor ...${NC}"
    parallel_remote_stop_processes ${VISOR_HOSTS} ${VISOR_BIN_FILE_NAME}

    parallel_remote_check_all

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Stop done${NC}"
}

kill() {
    echo -e "${INFO}Killing ...${NC}"

    # kill Keeper
    echo -e "${DEBUG}Killing ChronoKeeper ...${NC}"
    parallel_remote_kill_processes ${KEEPER_HOSTS} ${KEEPER_BIN_FILE_NAME}

    # kill Grapher
    echo -e "${DEBUG}Killing ChronoGrapher ...${NC}"
    parallel_remote_kill_processes ${GRAPHER_HOSTS} ${GRAPHER_BIN_FILE_NAME}

    # kill Visor
    echo -e "${DEBUG}Killing ChronoVisor ...${NC}"
    parallel_remote_kill_processes ${VISOR_HOSTS} ${VISOR_BIN_FILE_NAME}

    parallel_remote_check_all

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Kill done${NC}"
}

clean() {
    echo -e "${INFO}Cleaning ...${NC}"

    # check if ChronoVisor is still running
    echo -e "${DEBUG}Checking if ChronoVisor is still running${NC}"
    [[ -n $(parallel_remote_check_processes ${VISOR_HOSTS} ${VISOR_BIN_FILE_NAME}) ]] && echo -e "${ERR}ChronoVisor is still running, please use stop (-s) to stop it first, exiting ...${NC}" >&2 && exit 1

    # check if Grapher and Keeper are still running in group
    echo -e "${DEBUG}Checking if ChronoGraphers and ChronoKeepers are still running${NC}"
    for i in $(seq 1 ${NUM_RECORDING_GROUP}); do
        echo -e "${DEBUG}Checking RecordingGroup ${i}:${NC}"
        grapher_hosts_file="${GRAPHER_HOSTS}.${i}"
        keeper_hosts_file="${KEEPER_HOSTS}.${i}"
        [[ -n $(parallel_remote_check_processes ${grapher_hosts_file} ${GRAPHER_BIN_FILE_NAME}) ]] && echo -e "${ERR}ChronoGrapher is still running, please use stop (-s) to stop it first, exiting ...${NC}" >&2 && exit 1
        [[ -n $(parallel_remote_check_processes ${keeper_hosts_file} ${KEEPER_BIN_FILE_NAME}) ]] && echo -e "${ERR}ChronoKeeper is still running, please use stop (-s) to stop it first, exiting ...${NC}" >&2 && exit 1
    done

    # clean generated conf and hosts files
    echo -e "${DEBUG}Removing conf and hosts files ...${NC}"
    rm -f ${KEEPER_HOSTS}*.*
    rm -f ${GRAPHER_HOSTS}*.*
    rm -f ${KEEPER_BIN}.*
    rm -f ${GRAPHER_BIN}.*
    rm -f ${MONITOR_DIR}/*.log

    # clean log files
    echo -e "${DEBUG}Removing log files ...${NC}"
    rm -f ${CONF_FILE}.*

    # clean generated output files
    echo -e "${DEBUG}Removing output files ...${NC}"
    rm -f ${OUTPUT_DIR}/*

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Clean done${NC}"
}

parse_args() {
    TEMP=$(getopt -o t:l:w:m:u:v:g:p:q:a:o:f:j:r:hbidsce --long build-type:install-dir:work-dir:monitor-dir:output-dir:visor-bin:,grapher-bin:,keeper-bin:,visor-hosts:,grapher-hosts:,keeper-hosts:,conf-file:,job-id:,record-groups:,help,build,install,start,stop,clean,verbose -- "$@")
    if [ $? != 0 ]; then
        echo -e "${ERR}Terminating ...${NC}" >&2
        exit 1
    fi

    # Note the quotes around '$TEMP': they are essential!
    eval set -- "$TEMP"
    while [[ $# -gt 0 ]]; do
        case "$1" in
        -t | --build-type)
            BUILD_TYPE="$2"
            if [[ "$BUILD_TYPE" != "Debug" && "$BUILD_TYPE" != "Release" ]]; then
                echo -e "${ERR}Invalid build type: $BUILD_TYPE. Must be 'Debug' or 'Release'.${NC}"
                usage
            fi
            shift 2 ;;
        -l | --install-dir)
            INSTALL_DIR=$(realpath "$2")
            shift 2 ;;
        -w | --work-dir)
            WORK_DIR=$(realpath "$2")
            LIB_DIR="${WORK_DIR}/lib"
            CONF_DIR="${WORK_DIR}/conf"
            MONITOR_DIR="${WORK_DIR}/monitor"
            OUTPUT_DIR="${WORK_DIR}/output"
            VISOR_BIN_FILE_NAME="chronovisor_server"
            KEEPER_BIN_FILE_NAME="chrono_keeper"
            VISOR_BIN="${WORK_DIR}/bin/${VISOR_BIN_FILE_NAME}"
            GRAPHER_BIN="${WORK_DIR}/bin/${GRAPHER_BIN_FILE_NAME}"
            KEEPER_BIN="${WORK_DIR}/bin/${KEEPER_BIN_FILE_NAME}"
            VISOR_BIN_DIR=$(dirname ${VISOR_BIN})
            GRAPHER_BIN_DIR=$(dirname ${GRAPHER_BIN})
            KEEPER_BIN_DIR=$(dirname ${KEEPER_BIN})
            CONF_FILE="${CONF_DIR}/default_conf.json"
            VISOR_ARGS="--config ${CONF_FILE}"
            GRAPHER_ARGS="--config ${CONF_FILE}"
            KEEPER_ARGS="--config ${CONF_FILE}"
            VISOR_HOSTS="${CONF_DIR}/hosts_visor"
            GRAPHER_HOSTS="${CONF_DIR}/hosts_grapher"
            KEEPER_HOSTS="${CONF_DIR}/hosts_keeper"
            [ -f "${GRAPHER_HOSTS}" ] && NUM_RECORDING_GROUP=$(wc -l <"${GRAPHER_HOSTS}") || NUM_RECORDING_GROUP=1
            mkdir -p ${MONITOR_DIR}
            mkdir -p ${OUTPUT_DIR}
            shift 2 ;;
        -m | --monitor-dir)
            MONITOR_DIR=$(realpath "$2")
            shift 2 ;;
        -u | --output-dir)
            OUTPUT_DIR=$(realpath "$2")
            mkdir -p ${OUTPUT_DIR}
            shift 2 ;;
        -v | --visor-bin)
            VISOR_BIN=$(realpath "$2")
            VISOR_BIN_FILE_NAME=$(basename ${VISOR_BIN})
            VISOR_BIN_DIR=$(dirname ${VISOR_BIN})
            shift 2 ;;
        -g | --grapher-bin)
            GRAPHER_BIN=$(realpath "$2")
            GRAPHER_BIN_FILE_NAME=$(basename ${GRAPHER_BIN})
            GRAPHER_BIN_DIR=$(dirname ${GRAPHER_BIN})
            shift 2 ;;
        -p | --keeper-bin)
            KEEPER_BIN=$(realpath "$2")
            KEEPER_BIN_FILE_NAME=$(basename ${KEEPER_BIN})
            KEEPER_BIN_DIR=$(dirname ${KEEPER_BIN})
            shift 2 ;;
        -q | --visor-hosts)
            VISOR_HOSTS=$(realpath "$2")
            shift 2 ;;
        -a | --grapher-hosts)
            GRAPHER_HOSTS=$(realpath "$2")
            shift 2 ;;
        -o | --keeper-hosts)
            KEEPER_HOSTS=$(realpath "$2")
            shift 2 ;;
        -f | --conf-file)
            CONF_FILE=$(realpath "$2")
            CONF_DIR=$(dirname ${CONF_FILE})
            shift 2 ;;
        -j | --job-id)
            JOB_ID="$2"
            shift 2 ;;
        -r | --record-groups)
            NUM_RECORDING_GROUP="$2"
            shift 2 ;;
        -h | --help)
            usage
            shift 2 ;;
        -b | --build)
            build=true
            shift ;;
        -i | --install)
            install=true
            shift ;;
        -d | --start)
            start=true
            shift ;;
        -s | --stop)
            stop=true
            shift ;;
        -c | --clean)
            clean=true
            shift ;;
        -e | --verbose)
            verbose=true
            shift ;;
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

    # Shift the arguments so that the non-option arguments are left
    shift $((OPTIND - 1))

    # Check if WORK_DIR is set
    if [[ -z "${WORK_DIR}" ]]; then
        echo -e "${ERR}WORK_DIR is mandatory. Please provide it using the -w or --work-dir option.${NC}"
        usage
        exit 1
    fi
}

parse_args "$@"

# Check if specified operation is allowed
check_op_validity

if ${build}; then
    build
elif ${install}; then
    install
elif ${start}; then
    start
elif ${stop}; then
    stop
elif ${clean}; then
    clean
fi

echo -e "${INFO}Done${NC}"
