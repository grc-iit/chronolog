#!/bin/bash
# shellcheck disable=SC2086

# Define some colors
ERR='\033[7;37m\033[41m'
INFO='\033[7;49m\033[92m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

WORK_DIR="/home/${USER}/chronolog"
LIB_DIR="${WORK_DIR}/lib"
CONF_DIR="${WORK_DIR}/conf"
OUTPUT_DIR="${WORK_DIR}/output"
VISOR_BIN_FILE_NAME="chronovisor_server"
GRAPHER_BIN_FILE_NAME="chrono_grapher"
KEEPER_BIN_FILE_NAME="chrono_keeper"
CLIENT_BIN_FILE_NAME="client_lib_multi_storytellers"
VISOR_BIN="${WORK_DIR}/bin/${VISOR_BIN_FILE_NAME}"
GRAPHER_BIN="${WORK_DIR}/bin/${GRAPHER_BIN_FILE_NAME}"
KEEPER_BIN="${WORK_DIR}/bin/${KEEPER_BIN_FILE_NAME}"
CLIENT_BIN="${WORK_DIR}/bin/${CLIENT_BIN_FILE_NAME}"
VISOR_BIN_DIR=$(dirname ${VISOR_BIN})
GRAPHER_BIN_DIR=$(dirname ${GRAPHER_BIN})
KEEPER_BIN_DIR=$(dirname ${KEEPER_BIN})
CLIENT_BIN_DIR=$(dirname ${CLIENT_BIN})
CONF_FILE="${CONF_DIR}/default_conf.json"
VISOR_ARGS="--config ${CONF_FILE}"
GRAPHER_ARGS="--config ${CONF_FILE}"
KEEPER_ARGS="--config ${CONF_FILE}"
CLIENT_ARGS="--config ${CONF_FILE}"
VISOR_HOSTS="${CONF_DIR}/hosts_visor"
GRAPHER_HOSTS="${CONF_DIR}/hosts_grapher"
KEEPER_HOSTS="${CONF_DIR}/hosts_keeper"
CLIENT_HOSTS="${CONF_DIR}/hosts_client"
[ -f "${GRAPHER_HOSTS}" ] && NUM_RECORDING_GROUP=$(wc -l < "${GRAPHER_HOSTS}") || NUM_RECORDING_GROUP=1
HOSTNAME_HS_NET_SUFFIX="-40g"
JOB_ID=""
deploy=false
stop=false
kill=false
reset=false
local=false
verbose=false

usage() {
    echo "Usage: $0 -d|--deploy Start ChronoLog deployment (default: false)
                               -s|--stop Stop ChronoLog deployment (default: false)
                               -k|--kill Terminate ChronoLog deployment (default: false)
                               -r|--reset Reset/cleanup ChronoLog deployment (default: false)
                               -w|--work_dir WORK_DIR (default: ~/chronolog)
                               -u|--output_dir OUTPUT_DIR (default: work_dir/output)
                               -v|--visor VISOR_BIN (default: work_dir/bin/chronovisor_server)
                               -g|--grapher GRAPHER_BIN (default: work_dir/bin/chrono_grapher)
                               -p|--keeper KEEPER_BIN (default: work_dir/bin/chrono_keeper)
                               -c|--client CLIENT_BIN (default: work_dir/bin/client_lib_multi_storytellers)
                               -i|--visor_hosts VISOR_HOSTS (default: work_dir/conf/hosts_visor)
                               -a|--grapher_hosts GRAPHER_HOSTS (default: work_dir/conf/hosts_grapher)
                               -o|--keeper_hosts KEEPER_HOSTS (default: work_dir/conf/hosts_keeper)
                               -t|--client_hosts CLIENT_HOSTS (default: work_dir/conf/hosts_client)
                               -f|--conf_file CONF_FILE (default: work_dir/conf/default_conf.json)
                               -j|--job_id JOB_ID (default: \"\", overwrites hosts files if set)
                               -n|--num_recording_group NUM_RECORDING_GROUP (default: #hosts in GRAPHER_HOSTS if exists, 1 otherwise, overwrites hosts files if set)
                               -e|--verbose Enable verbose output (default: false)
                               -h|--help Print this page"
    exit 1
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
    check_file_existence ${CLIENT_HOSTS}

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Check hosts files done${NC}"
}

check_bin_files() {
    echo -e "${INFO}Checking binary files...${NC}"
    check_file_existence ${VISOR_BIN}
    check_file_existence ${GRAPHER_BIN}
    check_file_existence ${KEEPER_BIN}
    check_file_existence ${CLIENT_BIN}

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

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Check base conf file done${NC}"
}

check_op_validity() {
    count=0
    [[ $deploy == true ]] && ((count++))
    [[ $stop == true ]] && ((count++))
    [[ $kill == true ]] && ((count++))
    [[ $reset == true ]] && ((count++))

    if [[ $count -ne 1 ]]; then
        echo -e "${ERR}Error: Please select exactly one operation in deploy (-d), stop (-s), kill (-k) and reset (-r).${NC}" >&2
        usage
    fi
}

check_recording_group_mapping() {
    echo -e "${INFO}Checking recording group mapping ...${NC}"
    local num_keepers
    local num_recording_group

    num_keepers=$(wc -l < ${KEEPER_HOSTS})
    num_recording_group=${NUM_RECORDING_GROUP}
    if [[ "${num_recording_group}" -lt 1 ]] || [[ "${num_recording_group}" -gt "${num_keepers}" ]] || (( num_keepers % num_recording_group != 0 )); then
        echo -e "${ERR}NUM_RECORDING_GROUP must be greater than 0, less than or equal to the number of keepers (${num_keepers}), and a divisor of the number of keepers (${num_keepers}), exiting ...${NC}" >&2
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
        if [ "$lib_path" == "$linked_to_lib_path" ]
        then
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
        if [[ -n ${lib} ]]
        then
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
    jq ".chrono_visor.VisorClientPortalService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} > tmp.json && mv tmp.json ${CONF_FILE}
    jq ".chrono_client.VisorClientPortalService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} > tmp.json && mv tmp.json ${CONF_FILE}
    jq ".chrono_visor.VisorKeeperRegistryService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} > tmp.json && mv tmp.json ${CONF_FILE}
    jq ".chrono_keeper.VisorKeeperRegistryService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} > tmp.json && mv tmp.json ${CONF_FILE}
    jq ".chrono_grapher.VisorRegistryService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} > tmp.json && mv tmp.json ${CONF_FILE}

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Update ChronoVisor IP done${NC}"
}

generate_conf_for_each_keeper() {
    local base_conf_file=$1
    local keeper_hosts_file=$2
    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generating conf files for all ChronoKeepers in ${keeper_hosts_file} based on conf file ${base_conf_file} ...${NC}"
    while IFS= read -r keeper_host; do
        remote_keeper_hostname=$(LD_LIBRARY_PATH=/lib/x86_64-linux-gnu/ ssh -n ${keeper_host} hostname)
        [[ -z "${remote_keeper_hostname}" ]] && echo -e "${ERR}Cannot get hostname from ${keeper_host}, exiting ...${NC}" >&2 && exit 1
        [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generating conf file ${base_conf_file}.keeper.${remote_keeper_hostname} for ChronoKeeper ${remote_keeper_hostname} ...${NC}"
        jq ".chrono_keeper.Logging.log.file = \"chrono_keeper.log.${remote_keeper_hostname}\"" "${base_conf_file}" > "${base_conf_file}.keeper.${remote_keeper_hostname}"
        keeper_ip=$(get_host_ip ${keeper_host})
        jq ".chrono_keeper.KeeperRecordingService.rpc.service_ip = \"${keeper_ip}\"" "${base_conf_file}.keeper.${remote_keeper_hostname}" > temp.json && mv temp.json "${base_conf_file}.keeper.${remote_keeper_hostname}"
        jq ".chrono_keeper.KeeperDataStoreAdminService.rpc.service_ip = \"${keeper_ip}\"" "${base_conf_file}.keeper.${remote_keeper_hostname}" > temp.json && mv temp.json "${base_conf_file}.keeper.${remote_keeper_hostname}"
    done < ${keeper_hosts_file}

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
        jq ".chrono_grapher.Logging.log.file = \"chrono_grapher.log.${remote_grapher_hostname}\"" "${base_conf_file}" > "${base_conf_file}.grapher.${remote_grapher_hostname}"
        #grapher_ip=$(get_host_ip ${grapher_host})
        #jq ".chrono_keeper.KeeperGrapherDrainService.rpc.service_ip = \"${grapher_ip}\"" "${base_conf_file}".grapher.${remote_grapher_hostname}" > temp.json && mv temp.json "${base_conf_file}.grapher.${remote_grapher_hostname}"
        #jq ".chrono_grapher.KeeperGrapherDrainService.rpc.service_ip = \"${grapher_ip}\"" "${base_conf_file}.grapher.${remote_grapher_hostname}" > temp.json && mv temp.json "${base_conf_file}.grapher.${remote_grapher_hostname}"
    done < ${grapher_hosts_file}

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
        num_graphers_in_hosts_file=$(wc -l < ${grapher_hosts_file})
        [[ ${num_graphers_in_hosts_file} -ne 1 ]] && echo -e "${ERR}Exactly one node in ${grapher_hosts_file} is expected, exiting ...${NC}" >&2 && exit 1
        grapher_hostname=$(head -1 ${grapher_hosts_file})
        grapher_ip=$(get_host_ip ${grapher_hostname})
        remote_grapher_hostname=$(LD_LIBRARY_PATH=/lib/x86_64-linux-gnu/ ssh -n ${grapher_hostname} hostname)
        [[ -z "${remote_grapher_hostname}" ]] && echo -e "${ERR}Cannot get hostname from ${grapher_hostname}, exiting ...${NC}" >&2 && exit 1
        [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Updating conf file for members in RecordingGroup ${i} ...${NC}"
        jq ".chrono_grapher.DataStoreAdminService.rpc.service_ip = \"${grapher_ip}\"" "${CONF_FILE}" > "${CONF_FILE}.${i}"
        jq ".chrono_keeper.RecordingGroup = ${i}" "${CONF_FILE}.${i}" > temp.json && mv temp.json "${CONF_FILE}.${i}"
        jq ".chrono_grapher.RecordingGroup = ${i}" "${CONF_FILE}.${i}" > temp.json && mv temp.json "${CONF_FILE}.${i}"
        jq ".chrono_keeper.KeeperGrapherDrainService.rpc.service_ip = \"${grapher_ip}\"" "${CONF_FILE}.${i}" > temp.json && mv temp.json "${CONF_FILE}.${i}"
        jq ".chrono_grapher.KeeperGrapherDrainService.rpc.service_ip = \"${grapher_ip}\"" "${CONF_FILE}.${i}" > temp.json && mv temp.json "${CONF_FILE}.${i}"
        jq ".chrono_grapher.Logging.log.file = \"chrono_grapher.log.${remote_grapher_hostname}\"" "${CONF_FILE}.${i}" > temp.json && mv temp.json "${CONF_FILE}.${i}"
        jq ".chrono_keeper.story_files_dir = \"${OUTPUT_DIR}\"" "${CONF_FILE}.${i}" > temp.json && mv temp.json "${CONF_FILE}.${i}"
        jq ".chrono_grapher.Extractors.story_files_dir = \"${OUTPUT_DIR}\"" "${CONF_FILE}.${i}" > temp.json && mv temp.json "${CONF_FILE}.${i}"

        generate_conf_for_each_keeper "${CONF_FILE}.${i}" "${keeper_hosts_file}"

        generate_conf_for_each_grapher "${CONF_FILE}.${i}" "${grapher_hosts_file}"
    done
    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Generate conf files for all RecordingGroups done${NC}"
}

prepare_hosts_for_recording_groups() {
    for i in $(seq 1 ${NUM_RECORDING_GROUP}); do
        echo -e "${DEBUG}Generating hosts file for RecordingGroup ${i}...${NC}"
        total_num_keepers=$(wc -l < ${KEEPER_HOSTS})
        num_keepers_in_group=$((total_num_keepers / NUM_RECORDING_GROUP))
        start_line_num_in_keeper_hosts=$(((i - 1) * num_keepers_in_group + 1))
        end_line_num_in_keeper_hosts=$((i * num_keepers_in_group))
        sed -n "${start_line_num_in_keeper_hosts},${end_line_num_in_keeper_hosts}p" < ${KEEPER_HOSTS} > ${KEEPER_HOSTS}.${i}
        sed -n "${i}p" < ${GRAPHER_HOSTS} > ${GRAPHER_HOSTS}.${i}
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
        fi
    fi

    if [[ -n "${hosts}" ]]; then
        # use the first node as Visor by default
        echo "${hosts}" | head -1 > ${VISOR_HOSTS}
        # use last num_recording_group nodes as Graphers by default
        echo "${hosts}" | tail -${NUM_RECORDING_GROUP} > ${GRAPHER_HOSTS}
        # use all nodes as Keepers by default
        echo "${hosts}" > ${KEEPER_HOSTS}
        # use all nodes as Clients by default
        echo "${hosts}" > ${CLIENT_HOSTS}
    fi

    check_recording_group_mapping

    prepare_hosts_for_recording_groups

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Prepare hosts file done${NC}"
}

install() {
    echo -e "${INFO}Installing ...${NC}"

    copy_shared_libs

    prepare_hosts

    check_bin_files

    update_visor_ip

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

    if [[ "${local}" == "false" ]]; then
        # use hostname suffix conf file only on Ares
        hostname_suffix=".\$(hostname)"
        if [[ "${verbose}" == "false" ]]; then
            # grep only on Ares with simple output
            simple_output_grep_keyword="ares-"
        fi
    fi

    bin_path=${bin_dir}/${bin_filename}
    LD_LIBRARY_PATH=/lib/x86_64-linux-gnu/ mpssh -f ${hosts_file} "cd ${bin_dir}; LD_LIBRARY_PATH=${lib_dir} nohup ${bin_path} ${args} > ${bin_filename}${hostname_suffix}.launch.log 2>&1 &" | grep "${simple_output_grep_keyword}" 2>&1
}

parallel_remote_stop_processes() {
    local hosts_file=$1
    local bin_filename=$2

    LD_LIBRARY_PATH=/lib/x86_64-linux-gnu/ mpssh -f ${hosts_file} "pkill --signal 15 -ef ${bin_filename}" | grep -e "->" | grep -v ssh 2>&1
    while [[ -n $(parallel_remote_check_processes ${hosts_file} ${bin_filename}) ]]; do
        echo -e "${DEBUG}Some processes are still running, waiting for 10 seconds ...${NC}"
        sleep 10
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
    echo -e "${DEBUG}Running ChronoVisor (only one is expected):${NC}"
    parallel_remote_check_processes ${VISOR_HOSTS} ${VISOR_BIN_FILE_NAME}

    # check Grapher and Keeper in group
    echo -e "${DEBUG}Running ChronoKeepers:${NC}"
    for i in $(seq 1 ${NUM_RECORDING_GROUP}); do
        grapher_hosts_file="${GRAPHER_HOSTS}.${i}"
        parallel_remote_check_processes ${grapher_hosts_file} ${GRAPHER_BIN_FILE_NAME}
        keeper_hosts_file="${KEEPER_HOSTS}.${i}"
        parallel_remote_check_processes ${keeper_hosts_file} ${KEEPER_BIN_FILE_NAME}
    done

    # check Client
    echo -e "${DEBUG}Running Client (may ended already):${NC}"
    parallel_remote_check_processes ${CLIENT_HOSTS} ${CLIENT_BIN_FILE_NAME}
}

deploy() {
    install

    echo -e "${INFO}Deploying ...${NC}"

    local hostname_suffix=""
    local simple_output_grep_keyword=""

    if [[ "${local}" == "false" ]]; then
        # use hostname suffix conf file only on Ares
        hostname_suffix=".\$(hostname)"
        if [[ "${verbose}" == "false" ]]; then
            # grep only on Ares with simple output
            simple_output_grep_keyword="ares-"
        fi
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
        echo -e "${DEBUG}Launching ChronoGraphers from ${grapher_hosts_file} using conf file ${grapher_conf_file} ...${NC}"
        GRAPHER_BIN="${GRAPHER_BIN_DIR}/${GRAPHER_BIN_FILE_NAME}"
        GRAPHER_ARGS="--config ${grapher_conf_file}"
        grapher_hosts_file="${GRAPHER_HOSTS}.${i}"
        parallel_remote_launch_processes ${GRAPHER_BIN_DIR} ${LIB_DIR} ${grapher_hosts_file} ${GRAPHER_BIN_FILE_NAME} "${GRAPHER_ARGS}"

        # launch Keeper
        local keeper_conf_file="${CONF_FILE}.${i}.keeper${hostname_suffix}"
        echo -e "${DEBUG}Launching ChronoKeepers from ${keeper_hosts_file} using conf file ${keeper_conf_file} ...${NC}"
        KEEPER_BIN="${KEEPER_BIN_DIR}/${KEEPER_BIN_FILE_NAME}"
        KEEPER_ARGS="--config ${keeper_conf_file}"
        keeper_hosts_file="${KEEPER_HOSTS}.${i}"
        parallel_remote_launch_processes ${KEEPER_BIN_DIR} ${LIB_DIR} ${keeper_hosts_file} ${KEEPER_BIN_FILE_NAME} "${KEEPER_ARGS}"
    done

    # launch Client
    echo -e "${DEBUG}Launching Clients from ${CLIENT_HOSTS} using conf file ${CONF_FILE} ...${NC}"
    CLIENT_BIN="${CLIENT_BIN_DIR}/${CLIENT_BIN_FILE_NAME}"
    CLIENT_ARGS="--config ${CONF_FILE}"
    parallel_remote_launch_processes ${CLIENT_BIN_DIR} ${LIB_DIR} ${CLIENT_HOSTS} ${CLIENT_BIN_FILE_NAME} "${CLIENT_ARGS}"

    parallel_remote_check_all

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Deploy done${NC}"
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

    # stop Client
    echo -e "${DEBUG}Stopping Client ...${NC}"
    parallel_remote_stop_processes ${CLIENT_HOSTS} ${CLIENT_BIN_FILE_NAME}

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

remove_generated_files() {
    echo -e "${DEBUG}Removing generated log and conf files ...${NC}"
    rm -f ${CONF_FILE}.*
    rm -f ${KEEPER_HOSTS}*.*
    rm -f ${GRAPHER_HOSTS}*.*
    rm -f ${WORK_DIR}/bin/*.launch.log
    rm -f ${KEEPER_BIN}.*
    rm -f ${GRAPHER_BIN}.*
    rm -f ${WORK_DIR}/bin/chrono_visor.log
    rm -f ${WORK_DIR}/bin/chrono_client.log
}

kill() {
    echo -e "${INFO}Killing ...${NC}"

    if [[ -z ${JOB_ID} ]]; then
        echo -e "${DEBUG}No JOB_ID provided, use hosts files in ${CONF_DIR}${NC}"
        check_hosts_files
    else
        echo -e "${DEBUG}JOB_ID is provided, prepare hosts file first${NC}"
        prepare_hosts
    fi

    # kill Client
    echo -e "${DEBUG}Killing Client ...${NC}"
    parallel_remote_kill_processes ${CLIENT_HOSTS} ${CLIENT_BIN_FILE_NAME}

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

reset() {
    echo -e "${INFO}Resetting ...${NC}"

    # kill
    kill

    # cleanup files
    remove_generated_files

    [[ "${verbose}" == "true" ]] && echo -e "${DEBUG}Reset done${NC}"
}

parse_args() {
    TEMP=$(getopt -o w:u:v:a:g:p:c:i:o:t:f:j:n:hdskre --long work_dir:output_dir:visor:,grapher:,keeper:,client:,visor_hosts:,grapher_hosts:,keeper_hosts:,client_hosts:,conf_file:,job_id:,num_recording_group:,help,deploy,stop,kill,reset,verbose -- "$@" )
    if [ $? != 0 ] ; then echo -e "${ERR}Terminating ...${NC}" >&2 ; exit 1 ; fi

    # Note the quotes around '$TEMP': they are essential!
    eval set -- "$TEMP"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -w|--work_dir)
                WORK_DIR=$(realpath "$2")
                LIB_DIR="${WORK_DIR}/lib"
                CONF_DIR="${WORK_DIR}/conf"
                VISOR_BIN_FILE_NAME="chronovisor_server"
                KEEPER_BIN_FILE_NAME="chrono_keeper"
                CLIENT_BIN_FILE_NAME="client_lib_multi_storytellers"
                VISOR_BIN="${WORK_DIR}/bin/${VISOR_BIN_FILE_NAME}"
                KEEPER_BIN="${WORK_DIR}/bin/${KEEPER_BIN_FILE_NAME}"
                CLIENT_BIN="${WORK_DIR}/bin/${CLIENT_BIN_FILE_NAME}"
                VISOR_BIN_DIR=$(dirname ${VISOR_BIN})
                KEEPER_BIN_DIR=$(dirname ${KEEPER_BIN})
                CLIENT_BIN_DIR=$(dirname ${CLIENT_BIN})
                CONF_FILE="${CONF_DIR}/default_conf.json"
                VISOR_ARGS="--config ${CONF_FILE}"
                KEEPER_ARGS="--config ${CONF_FILE}"
                CLIENT_ARGS="--config ${CONF_FILE}"
                VISOR_HOSTS="${CONF_DIR}/hosts_visor"
                KEEPER_HOSTS="${CONF_DIR}/hosts_keeper"
                CLIENT_HOSTS="${CONF_DIR}/hosts_client"
                shift 2 ;;
            -u|--output_dir)
                OUTPUT_DIR=$(realpath "$2")
                mkdir -p ${OUTPUT_DIR}
                shift 2 ;;
            -v|--visor)
                VISOR_BIN=$(realpath "$2")
                VISOR_BIN_FILE_NAME=$(basename ${VISOR_BIN})
                VISOR_BIN_DIR=$(dirname ${VISOR_BIN})
                shift 2 ;;
            -g|--grapher)
                GRAPHER_BIN=$(realpath "$2")
                GRAPHER_BIN_FILE_NAME=$(basename ${GRAPHER_BIN})
                GRAPHER_BIN_DIR=$(dirname ${GRAPHER_BIN})
                shift 2 ;;
            -p|--keeper)
                KEEPER_BIN=$(realpath "$2")
                KEEPER_BIN_FILE_NAME=$(basename ${KEEPER_BIN})
                KEEPER_BIN_DIR=$(dirname ${KEEPER_BIN})
                shift 2 ;;
            -c|--client)
                CLIENT_BIN=$(realpath "$2")
                CLIENT_BIN_FILE_NAME=$(basename ${CLIENT_BIN})
                CLIENT_BIN_DIR=$(dirname ${CLIENT_BIN})
                shift 2 ;;
            -i|--visor_hosts)
                VISOR_HOSTS=$(realpath "$2")
                shift 2 ;;
            -a|--grapher_hosts)
                GRAPHER_HOSTS=$(realpath "$2")
                shift 2 ;;
            -o|--keeper_hosts)
                KEEPER_HOSTS=$(realpath "$2")
                shift 2 ;;
            -t|--client_hosts)
                CLIENT_HOSTS=$(realpath "$2")
                shift 2 ;;
            -f|--conf_file)
                CONF_FILE=$(realpath "$2")
                CONF_DIR=$(dirname ${CONF_FILE})
                shift 2 ;;
            -j|--job_id)
                JOB_ID="$2"
                shift 2 ;;
            -n|--num_recording_group)
                NUM_RECORDING_GROUP="$2"
                shift 2 ;;
            -h|--help)
                usage
                shift 2 ;;
            -d|--deploy)
                deploy=true
                shift ;;
            -s|--stop)
                stop=true
                shift ;;
            -k|--kill)
                kill=true
                shift ;;
            -r|--reset)
                reset=true
                shift ;;
            #-l|--local)
                #local=true
                #HOSTNAME_HS_NET_SUFFIX=""
                #shift ;;
            -e|--verbose)
                verbose=true
                shift ;;
            --)
                shift; break ;;
            *)
                echo -e "${ERR}Unknown option: $1${NC}"
                exit 1
                ;;
        esac
    done

    # Shift the arguments so that the non-option arguments are left
    shift $((OPTIND - 1))
}

parse_args "$@"

# Check if specified operation is allowed
check_op_validity

if ${deploy}; then
    deploy
elif ${stop}; then
    stop
elif ${kill}; then
    kill
elif ${reset}; then
    reset
fi

echo -e "${INFO}Done${NC}"
