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
OUTPUT_DIR="${WORK_DIR}/output"

# Files
VISOR_BIN="${BIN_DIR}/chronovisor_server"
KEEPER_BIN="${BIN_DIR}/chrono_keeper"
CLIENT_BIN="${BIN_DIR}/client_lib_multi_storytellers"
GRAPHER_BIN="${BIN_DIR}/chrono_grapher"
CONF_FILE="${CONF_DIR}/default_conf.json"

#Booleans
deploy=false
reset=false
stop=false
kill=false

# Methods ______________________________________________________________________________________________________________

check_dependencies() {
    local dependencies=("jq" "ldd" "nohup" "pkill" "readlink")

    echo -e "${INFO}Checking required dependencies...${NC}"
    for dep in "${dependencies[@]}"; do
        if ! command -v $dep &> /dev/null; then
            echo -e "${ERR}Dependency $dep is not installed. Please install it and try again.${NC}"
            exit 1
        fi
    done
    echo -e "${DEBUG}All required dependencies are installed.${NC}"
}

check_directories() {
    echo -e "${INFO}Checking required directories...${NC}"
    local directories=("${WORK_DIR}" "${LIB_DIR}" "${CONF_DIR}" "${BIN_DIR}" "${OUTPUT_DIR}")

    for dir in "${directories[@]}"; do
        if [[ ! -d ${dir} ]]; then
            echo -e "${ERR}Directory ${dir} does not exist. Please create it and try again.${NC}"
            exit 1
        fi
    done
    echo -e "${DEBUG}All required directories are in place.${NC}"
}

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
    local output_dir=$5
    local num_recording_groups=$6

    # Check if default configuration file exists
    if [ ! -f "$default_conf" ]; then
        echo "Default configuration file $default_conf not found."
        return 1
    fi

    # Check if number of keepers and graphers are valid
    if (( NUM_KEEPERS <= 0 || NUM_GRAPHERS <= 0 )); then
        echo "Number of keepers and graphers must be greater than 0. Exiting..."
        exit
    fi

    if (( NUM_GRAPHERS > NUM_KEEPERS )); then
        echo "Number of graphers must be less than or equal to the number of keepers ($NUM_KEEPERS). Exiting..."
        exit
    fi

    if (( NUM_KEEPERS % NUM_GRAPHERS != 0 )); then
        echo "Number of graphers must be a divisor of the number of keepers ($NUM_KEEPERS). Exiting..."
        exit
    fi

    # Calculate number of keepers per grapher
    keepers_per_grapher=$((NUM_KEEPERS / NUM_GRAPHERS))

    # Extract initial ports from the configuration file
    local base_port_keeper_record=$(jq '.chrono_keeper.KeeperRecordingService.rpc.service_base_port' "$default_conf")
    local base_port_keeper_drain=$(jq '.chrono_keeper.KeeperGrapherDrainService.rpc.service_base_port' "$default_conf")
    local base_port_keeper_datastore=$(jq '.chrono_keeper.KeeperDataStoreAdminService.rpc.service_base_port' "$default_conf")

    # Generate keeper configuration files
    for (( i=0; i<num_files; i++ )); do
        local new_port_keeper_record=$((base_port_keeper_record + i))
        local new_port_keeper_datastore=$((base_port_keeper_datastore + i))
        local recording_group=$((i / keepers_per_grapher))

        j=$((i + 1))
        recording_group=$((recording_group+1))
        local new_port_keeper_drain=$((base_port_keeper_drain + recording_group))

        local output_file="${conf_dir}/keeper_conf_${j}.json"

        jq --arg bin_dir "$bin_dir" \
            --arg output_dir "$output_dir" \
            --argjson new_port_keeper_record $new_port_keeper_record \
            --argjson new_port_keeper_drain $new_port_keeper_drain \
            --argjson new_port_keeper_datastore $new_port_keeper_datastore \
            --argjson recording_group "$recording_group" \
            --arg j "$j" \
            '.chrono_keeper.KeeperRecordingService.rpc.service_base_port = $new_port_keeper_record |
            .chrono_keeper.KeeperGrapherDrainService.rpc.service_base_port = $new_port_keeper_drain |
            .chrono_keeper.KeeperDataStoreAdminService.rpc.service_base_port = $new_port_keeper_datastore |
            .chrono_keeper.story_files_dir = ($output_dir + "/") |
            .chrono_keeper.RecordingGroup = $recording_group |
            .chrono_keeper.Logging.log.file = ($bin_dir + "/" + $j + "_" + .chrono_keeper.Logging.log.file)' "$default_conf" > "$output_file"
        echo "Generated $output_file with ports $new_port_keeper_record and $new_port_keeper_datastore and $new_port_keeper_drain"
    done

    # Generate visor configuration file
    local visor_output_file="${conf_dir}/visor_conf.json"
    jq --arg bin_dir "$bin_dir" \
       '.chrono_visor.Logging.log.file = ($bin_dir + "/" + .chrono_visor.Logging.log.file)' "$default_conf" > "$visor_output_file"
    echo "Generated $visor_output_file"

    # Generate client configuration file
    local client_output_file="${conf_dir}/client_conf.json"
    jq --arg bin_dir "$bin_dir" \
       '.chrono_client.Logging.log.file = ($bin_dir + "/" + .chrono_client.Logging.log.file)' "$default_conf" > "$client_output_file"
    echo "Generated $client_output_file"

    local base_port_grapher_drain=$(jq '.chrono_grapher.KeeperGrapherDrainService.rpc.service_base_port' "$default_conf")
    local base_port_grapher_datastore=$(jq '.chrono_grapher.DataStoreAdminService.rpc.service_base_port' "$default_conf")

    # Generate grapher configuration files
    echo "Generating grapher conf files ..."
    mkdir -p "${output_dir}"
    for (( i=1; i<num_recording_groups+1; i++ )); do
        local new_port_grapher_drain=$((base_port_grapher_drain + i))
        local new_port_grapher_datastore=$((base_port_grapher_datastore + i))

        local grapher_output_file="${conf_dir}/grapher_conf_${i}.json"
        j=$i
        jq --arg bin_dir "$bin_dir" \
            --arg output_dir "$output_dir" \
            --argjson new_port_grapher_drain $new_port_grapher_drain \
            --argjson new_port_grapher_datastore $new_port_grapher_datastore \
            --argjson j "$j" \
            --arg i "$i" \
           '.chrono_grapher.RecordingGroup = $j |
            .chrono_grapher.KeeperGrapherDrainService.rpc.service_base_port = $new_port_grapher_drain |
            .chrono_grapher.DataStoreAdminService.rpc.service_base_port = $new_port_grapher_datastore |
            .chrono_grapher.Logging.log.file = ($bin_dir + "/" + $i + "_" + .chrono_grapher.Logging.log.file) |
            .chrono_grapher.Extractors.story_files_dir = ($output_dir + "/")' "$default_conf" > "$grapher_output_file"

        echo "Generated $grapher_output_file with ports $new_port_grapher_drain and $new_port_grapher_datastore"
    done

    echo "Generate conf files for all RecordingGroups done"
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
    # Copy shared libraries to the lib directory
    echo -e "${DEBUG}Copying shared libraries ...${NC}"
    mkdir -p ${LIB_DIR}

    all_shared_libs=""
    for bin_file in "${BIN_DIR}"/*; do
        echo -e "${DEBUG}Extracting shared libraries from ${bin_file} ...${NC}";
        all_shared_libs=$(echo -e "${all_shared_libs}\n$(extract_shared_libraries ${bin_file})" | sort | uniq)
    done

    for lib in ${all_shared_libs}; do
        if [[ -n ${lib} ]]
        then
            copy_shared_libs_recursive ${lib} ${LIB_DIR}
        fi
    done

    echo -e "${DEBUG}Copy shared libraries done${NC}"
}

# Functions to launch and kill processes
launch_process() {
    local bin="$1"
    local args="$2"
    local log_file="$3"
    echo -e "${DEBUG}Launching $bin $args ...${NC}"
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
    check_dependencies
    check_directories
    check_files
    copy_shared_libs
    echo "Output dir2 ${OUTPUT_DIR}"
    generate_config_files ${NUM_KEEPERS} ${BIN_DIR} ${CONF_FILE} ${CONF_DIR} ${OUTPUT_DIR} ${NUM_GRAPHERS} ${WORK_DIR}
    echo -e "${DEBUG}Install done${NC}"
}

deploy() {
    echo -e "${INFO}Deploying ...${NC}"
    install
    launch_process ${VISOR_BIN} "--config ${CONF_DIR}/visor_conf.json" "visor.log"
    sleep 2
    num_keepers=${NUM_KEEPERS}
    for (( i=1; i<num_keepers+1; i++ ))
    do
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
    echo -e "${DEBUG}Deployment done${NC}"
}

kill() {
    echo -e "${INFO}Killing ...${NC}"
    kill_process ${CLIENT_BIN}
    kill_process ${KEEPER_BIN}
    kill_process ${GRAPHER_BIN}
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
    echo "  -h|--help       Display this help and exit"

    echo "  -d|--deploy     Start ChronoLog Deployment (default: false)"
    echo "  -s|--stop       Stop ChronoLog Deployment (default: false)"
    echo "  -r|--reset      Reset/CleanUp ChronoLog Deployment (default: false)"
    echo "  -k|--kill       Terminate ChronoLog Deployment (default: false)"

    echo "  -w|--work-dir DIR     Set the working directory"
    echo "  -u|--output_dir OUTPUT_DIR (default: work_dir/output)"
    echo "  -v|--visor VISOR_BIN (default: work_dir/bin/chronovisor_server)"
    echo "  -g|--grapher GRAPHER_BIN (default: work_dir/bin/chrono_grapher)"
    echo "  -p|--keeper KEEPER_BIN (default: work_dir/bin/chrono_keeper)"
    echo "  -c|--client CLIENT_BIN (default: work_dir/bin/client_lib_multi_storytellers)"
    echo "  -f|--conf_file CONF_FILE (default: work_dir/conf/default_conf.json)"

    echo "  -n|--keepers-group  Set the number of keeper processes per group"
    echo "  -j|--recording-groups Set the number of recording groups or grapher processes"

    exit 1
}

# Parse arguments to set variables
parse_args() {
    while [[ "$#" -gt 0 ]]; do
        case "$1" in
            -d|--deploy)
                deploy=true
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
            -w|--work-dir)
                WORK_DIR="$2";
                LIB_DIR="${WORK_DIR}/lib"
                CONF_DIR="${WORK_DIR}/conf"
                BIN_DIR="${WORK_DIR}/bin"
                OUTPUT_DIR="${WORK_DIR}/output"
                VISOR_BIN="${BIN_DIR}/chronovisor_server"
                KEEPER_BIN="${BIN_DIR}/chrono_keeper"
                CLIENT_BIN="${BIN_DIR}/client_lib_multi_storytellers"
                GRAPHER_BIN="${BIN_DIR}/chrono_grapher"
                CONF_FILE="${CONF_DIR}/default_conf.json"
                shift 2 ;;
            -u|--output-dir)
                OUTPUT_DIR=$(realpath "$2")
                mkdir -p ${OUTPUT_DIR}
                shift 2 ;;
            -v|--visor)
                VISOR_BIN=$(realpath "$2")
                shift 2 ;;
            -g|--grapher)
                GRAPHER_BIN=$(realpath "$2")
                shift 2 ;;
            -p|--keeper)
                KEEPER_BIN=$(realpath "$2")
                shift 2 ;;
            -c|--client)
                CLIENT_BIN=$(realpath "$2")
                shift 2 ;;
            -f|--conf_file)
                CONF_FILE=$(realpath "$2")
                CONF_DIR=$(dirname ${CONF_FILE})
                shift 2 ;;
            -j|--recording-groups)
                NUM_GRAPHERS="$2"
                NUM_KEEPERS=$((NUM_KEEPERS * NUM_GRAPHERS))
                shift 2 ;;
            -n|--keepers-group)
                keepers_group="$2"
                NUM_KEEPERS=$((keepers_group * NUM_GRAPHERS))
                shift 2 ;;
            *) echo -e "${ERR}Unknown option: $1${NC}"; usage ;;
        esac
    done
}

# Start execution of the script____________________________________________________________________________________
parse_args "$@"
if ${deploy}
then
    deploy
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

