#!/bin/bash

# Variables ____________________________________________________________________________________________________________
# Define some colors
ERR='\033[1;31m\033[41m'
INFO='\033[1,36m\033[42m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color
NUM_KEEPERS=1
NUM_GRAPHERS=1

# Directories
WORK_DIR="/home/${USER}/chronolog"
LIB_DIR="${WORK_DIR}/lib"
CONF_DIR="${WORK_DIR}/conf"
BIN_DIR="${WORK_DIR}/bin"

# Files
VISOR_BIN="${BIN_DIR}/chronovisor_server"
KEEPER_BIN="${BIN_DIR}/chrono_keeper"
CLIENT_BIN="${BIN_DIR}/client_lib_multi_storytellers"
GRAPHER_BIN="${BIN_DIR}/chrono_grapher"
CONF_FILE="${CONF_DIR}/default_conf.json"

#Booleans
install=false
reset=false
stop=false
kill=false

# Methods ______________________________________________________________________________________________________________

check_files() {
    echo -e "${INFO}Checking required files...${NC}"
    [[ ! -f ${VISOR_BIN} ]] && echo -e "${ERR}Visor binary file does not exist, exiting ...${NC}" && exit 1
    [[ ! -f ${KEEPER_BIN} ]] && echo -e "${ERR}Keeper binary file does not exist, exiting ...${NC}" && exit 1
    [[ ! -f ${CLIENT_BIN} ]] && echo -e "${ERR}Client binary file does not exist, exiting ...${NC}" && exit 1
    [[ ! -f ${GRAPHER_BIN} ]] && echo -e "${ERR}Grapher binary file does not exist, exiting ...${NC}" && exit 1
    [[ ! -f ${CONF_FILE} ]] && echo -e "${ERR}Configuration file does not exist, exiting ...${NC}" && exit 1
    echo -e "${DEBUG}All required files are in place.${NC}"
}

generate_config_files() {
    local num_files=$1
    local bin_dir=$2
    local default_conf=$3
    local conf_dir=$4

    # Check if default configuration file exists
    if [ ! -f "$default_conf" ]; then
        echo "Default configuration file $default_conf not found."
        return 1
    fi

    # Extract initial ports from the configuration file
    local base_port_keeper_record=$(jq '.chrono_keeper.KeeperRecordingService.rpc.service_base_port' "$default_conf")
    local base_port_keeper_datastore=$(jq '.chrono_keeper.KeeperDataStoreAdminService.rpc.service_base_port' "$default_conf")

    # Generate configuration files
    for (( i=0; i<num_files; i++ ))
    do
        # Calculate new ports
        local new_port_keeper_record=$((base_port_keeper_record + i))
        local new_port_keeper_datastore=$((base_port_keeper_datastore + i))

        # Create new config file name
        local output_file="${conf_dir}/keeper_conf_$i.json"

        # Use jq to modify the JSON
        jq --arg bin_dir "$bin_dir" \
           --argjson new_port_keeper_record $new_port_keeper_record \
           --argjson new_port_keeper_datastore $new_port_keeper_datastore \
           --arg i "$i" \
           '.chrono_keeper.KeeperRecordingService.rpc.service_base_port = $new_port_keeper_record |
            .chrono_keeper.KeeperDataStoreAdminService.rpc.service_base_port = $new_port_keeper_datastore |
            .chrono_keeper.Logging.log.file = ($bin_dir + "/" + $i + "_" + .chrono_keeper.Logging.log.file)' "$default_conf" > "$output_file"

        echo "Generated $output_file with ports $new_port_keeper_record and $new_port_keeper_datastore."
    done

    local output_file="${conf_dir}/visor_conf.json"
    jq --arg bin_dir "$bin_dir" \
       '.chrono_visor.Logging.log.file = ($bin_dir + "/" + .chrono_visor.Logging.log.file)' "$default_conf" > "$output_file"

    local output_file="${conf_dir}/client_conf.json"
    jq --arg bin_dir "$bin_dir" \
       '.chrono_client.Logging.log.file = ($bin_dir + "/" + .chrono_client.Logging.log.file)' "$default_conf" > "$output_file"


}

extract_shared_libraries() {
    local executable="$1"
    ldd_output=$(ldd ${executable} 2>/dev/null | grep '=>' | awk '{print $3}' | grep -v 'not' | grep -v '^/lib')
    echo "${ldd_output}"
}

copy_shared_libs_recursive() {
    local lib_path="$1"
    local dest_path="$2"
    local linked_to_lib_path="$(readlink -f ${lib_path})"

    # Copy the library and maintain symbolic links recursively
    final_dest_lib_copies=false
    echo -e "${DEBUG}Copying ${lib_path} recursively ...${NC}"
    while [ "$final_dest_lib_copies" != true ]
    do
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
    libs_visor=$(extract_shared_libraries ${VISOR_BIN})
    libs_keeper=$(extract_shared_libraries ${KEEPER_BIN})
    libs_client=$(extract_shared_libraries ${CLIENT_BIN})

    # Combine shared libraries from all executables and remove duplicates
    all_shared_libs=$(echo -e "${libs_visor}\n${libs_keeper}\n${libs_client}" | sort | uniq)

    # Copy shared libraries to the lib directory
    echo -e "${DEBUG}Copying shared library ...${NC}"
    mkdir -p ${LIB_DIR}
    for lib in ${all_shared_libs}; do
        if [[ ! -z ${lib} ]]
        then
            copy_shared_libs_recursive ${lib} ${LIB_DIR}
        fi
    done
}

# Functions to launch and kill processes
launch_process() {
    local bin="$1"
    local args="$2"
    local log_file="$3"
    echo -e "${DEBUG}Launching $bin ...${NC}"
    LD_LIBRARY_PATH=${LIB_DIR} nohup ${bin} ${args} > ${BIN_DIR}/${log_file} 2>&1 &
}

kill_process() {
    local bin="$1"
    echo -e "${DEBUG}Killing $(basename ${bin}) ...${NC}"
    pkill -9 -f ${bin}
}

stop_process() {
    local bin="$1"
    echo -e "${DEBUG}Stopping $(basename ${bin}) ...${NC}"
    pkill -f ${bin}
}

# Main functions for install, reset, and usage
install() {
    echo -e "${INFO}Installing ...${NC}"
    copy_shared_libs
    check_files
    generate_config_files ${NUM_KEEPERS} ${BIN_DIR} ${CONF_FILE} ${CONF_DIR} "/home/eneko/chronolog/output/" ${NUM_GRAPHERS} ${WORK_DIR}
    launch_process ${VISOR_BIN} "--config ${CONF_DIR}/visor_conf.json" "visor.log"
    sleep 2
    num_keepers=${NUM_KEEPERS}
    for (( i=0; i<num_keepers; i++ ))
    do
        #local keeper_args="--config ${CONF_DIR}/keeper_conf_$i.json"
        launch_process ${KEEPER_BIN} "--config ${CONF_DIR}/keeper_conf_$i.json" "keeper_$i.log"
    done
    sleep 2
    num_graphers=${NUM_GRAPHERS}
    for (( i=1; i<num_graphers+1; i++ ))
    do
        launch_process ${GRAPHER_BIN} "--config ${CONF_DIR}/grapher_conf_$i.json" "grapher_$i.log"
    done
    sleep 2
    launch_process ${CLIENT_BIN} "--config ${CONF_DIR}/client_conf.json" "client.log"
    echo -e "${DEBUG}Install done${NC}"
}

kill() {
    echo -e "${INFO}Killing ...${NC}"
    kill_process ${VISOR_BIN}
    echo -e "${DEBUG}Kill done${NC}"
}

reset() {
    echo -e "${INFO}Resetting...${NC}"
    kill;
    echo -e "${DEBUG}Delete all generated files${NC}"

    # Remove all config files
    rm ${CONF_DIR}/client_conf.json
    rm ${CONF_DIR}/grapher_conf*.json
    rm ${CONF_DIR}/keeper_conf*.json
    rm ${CONF_DIR}/group_conf*.json
    rm ${CONF_DIR}/visor_conf.json

    # Remove all log files
    rm ${BIN_DIR}/*.log
    echo -e "${DEBUG}Reset done${NC}"
}

stop() {
    echo -e "${INFO}Stopping ...${NC}"
    declare -a processes=("${CLIENT_BIN}" "${KEEPER_BIN}" "${GRAPHER_BIN}" "${VISOR_BIN}")
    for process in "${processes[@]}"; do
        stop_process "${process}"
        while pgrep -f "${process}" >/dev/null; do
            echo -e "${DEBUG}Waiting for ${process} to stop...${NC}"
            sleep 10
        done
        echo -e "${DEBUG}${process} stop done${NC}"
    done
    echo -e "${DEBUG}All processes stopped${NC}"
}

# Usage function with new options
usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -h, --help             Display this help and exit"
    echo "  -n, --num-keepers NUM  Set the number of keeper processes"
    echo "  -g, --num-graphers NUM Set the number of grapher processes"
    echo "  -w, --work-dir DIR     Set the working directory"
    echo "  -i, --install          Install all components"
    echo "  -r, --reset            Reset all components"
    echo "  -k, --kill             Kill all components"
    echo "  -s, --stop             Stop all components"
    exit 1
}

# Parse arguments to set variables
parse_args() {
    while [[ "$#" -gt 0 ]]; do
        case "$1" in
            -i|--install)
                install=true
                shift ;;
            -r|--reset)
                reset=true
                shift ;;
            -k|--kill)
                kill=true
                shift ;;
            -s|--stop)
                stop=true
                shift ;;
            -h|--help)
                usage;
                shift ;;
            -n|--num-keepers)
                NUM_KEEPERS="$2";
                shift 2 ;;
            -g|--num-graphers)
                NUM_GRAPHERS="$2"
                shift 2 ;;
            -w|--work-dir)
                WORK_DIR="$2";
                LIB_DIR="${WORK_DIR}/lib"
                CONF_DIR="${WORK_DIR}/conf"
                BIN_DIR="${WORK_DIR}/bin"
                VISOR_BIN="${BIN_DIR}/chronovisor_server"
                KEEPER_BIN="${BIN_DIR}/chrono_keeper"
                CLIENT_BIN="${BIN_DIR}/client_lib_multi_storytellers"
                GRAPHER_BIN="${BIN_DIR}/chrono_grapher"
                CONF_FILE="${CONF_DIR}/default_conf.json"
                shift 2 ;;
            *) echo -e "${ERR}Unknown option: $1${NC}"; usage ;;
        esac
    done
}

# Start execution of the script____________________________________________________________________________________
parse_args "$@"
if ${install}
then
    install
elif ${reset}
then
    reset
elif ${stop}
then
    stop
elif ${kill}
then
    kill
else
    echo -e "${ERR}Please select deploy or reset mode${NC}"
    usage
fi

echo -e "${INFO}Done${NC}"

