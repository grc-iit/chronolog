#!/bin/bash

# Variables ____________________________________________________________________________________________________________
# Define some colors
ERR='\033[7;37m\033[41m'
INFO='\033[7;49m\033[92m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

# Default values
NUM_KEEPERS=1
NUM_RECORDING_GROUPS=1
BUILD_TYPE="Release"

# Directories
WORK_DIR=""
LIB_DIR=""
CONF_DIR=""
BIN_DIR=""
MONITOR_DIR=""
OUTPUT_DIR=""
INSTALL_DIR=""
REPO_ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/..")"

# Files
VISOR_BIN="${BIN_DIR}/chronovisor_server"
KEEPER_BIN="${BIN_DIR}/chrono_keeper"
GRAPHER_BIN="${BIN_DIR}/chrono_grapher"
PLAYER_BIN="${BIN_DIR}/chrono_player"
CONF_FILE="${CONF_DIR}/default_conf.json"

#Booleans
build=false
install=false
start=false
stop=false
clean=false

EXEC_MODE_COUNT=0

# Helper Methods _______________________________________________________________________________________________________
start_service() {
    local bin="$1"
    local args="$2"
    local monitor_file="$3"
    echo -e "${DEBUG}Launching $bin $args ...${NC}"
    LD_LIBRARY_PATH=${LIB_DIR} nohup ${bin} ${args} > ${MONITOR_DIR}/${monitor_file} 2>&1 &
}

kill_service() {
    local bin=$(basename "$1")
    echo -e "${DEBUG}Killing $(basename ${bin}) ...${NC}"
    pkill -9 -f ${bin}
}

stop_service() {
    local bin=$(basename "$1") 
    local timeout="$2"

    # Stop all processes of the service in parallel
    local start_time=$(date +%s)
    echo -e "${DEBUG}Stopping $(basename ${bin}) ...${NC}"
    pkill -f ${bin}

    # Wait for processes to stop with a timeout
    while true; do
        if pgrep -f "${bin}" >/dev/null; then
            echo -e "${DEBUG}Waiting for ${bin} to stop...${NC}"
        else
            echo -e "${DEBUG}All service processes stopped gracefully.${NC}"
            break
        fi
        sleep 10
        # Check if timeout is reached
        local current_time=$(date +%s)
        if (( current_time - start_time >= timeout )); then
            echo -e "${DEBUG}Timeout reached while stopping processes. Forcing termination.${NC}"
            if pgrep -f "${bin}" >/dev/null; then
                kill_service ${bin}
                echo -e "${DEBUG}Killed: ${bin} ${NC}"
            fi
            break
        fi
    done
    echo ""
}



generate_config_files() {
    local num_keepers=$1
    local default_conf=$2
    local conf_dir=$3
    local output_dir=$4
    local num_recording_groups=$5
    local monitor_dir=$6

    mkdir -p "${monitor_dir}"

    # Check if default configuration file exists
    if [ ! -f "$default_conf" ]; then
        echo "Default configuration file $default_conf not found."
        exit 1
    fi

    # Check if number of keepers and graphers are valid
    if (( num_keepers <= 0 || num_recording_groups <= 0 )); then
        echo "Number of keepers and graphers must be greater than 0. Exiting..."
        exit 1
    fi

    if (( num_keepers < num_recording_groups )); then
        echo "Number of keepers must be greater than or equal to the number of graphers. Exiting..."
        exit 1
    fi

    # Extract initial ports from the configuration file
    local base_port_keeper_record=$(jq -r '.chrono_keeper.KeeperRecordingService.rpc.service_base_port' "$default_conf")
    local base_port_keeper_drain=$(jq -r '.chrono_keeper.KeeperGrapherDrainService.rpc.service_base_port' "$default_conf")
    local base_port_keeper_datastore=$(jq -r '.chrono_keeper.KeeperDataStoreAdminService.rpc.service_base_port' "$default_conf")
    local base_port_grapher_drain=$(jq -r '.chrono_grapher.KeeperGrapherDrainService.rpc.service_base_port' "$default_conf")
    local base_port_grapher_datastore=$(jq -r '.chrono_grapher.DataStoreAdminService.rpc.service_base_port' "$default_conf")
    local base_port_player_datastore=$(jq -r '.chrono_player.PlayerStoreAdminService.rpc.service_base_port' "$default_conf")
    local base_port_player_playback=$(jq -r '.chrono_player.PlaybackQueryService.rpc.service_base_port' "$default_conf")

    # Generate grapher configuration files
    echo "Generating grapher configuration files ..."
    mkdir -p "${output_dir}"
    for (( i=0; i<num_recording_groups; i++ )); do
        local new_port_grapher_drain=$((base_port_grapher_drain + i))
        local new_port_grapher_datastore=$((base_port_grapher_datastore + i))

        local grapher_index=$((i + 1))
        local grapher_output_file="${conf_dir}/grapher_conf_${grapher_index}.json"

        grapher_monitoring_file=$(jq -r '.chrono_grapher.Monitoring.monitor.file' "$default_conf")
        grapher_monitoring_file_name=$(basename "$grapher_monitoring_file")
        jq --arg monitor_dir "$monitor_dir" \
            --arg output_dir "$output_dir" \
            --argjson new_port_grapher_drain $new_port_grapher_drain \
            --argjson new_port_grapher_datastore $new_port_grapher_datastore \
            --argjson grapher_index "$grapher_index" \
            --arg grapher_monitoring_file_name "$grapher_monitoring_file_name" \
           '.chrono_grapher.RecordingGroup = $grapher_index |
            .chrono_grapher.KeeperGrapherDrainService.rpc.service_base_port = $new_port_grapher_drain |
            .chrono_grapher.DataStoreAdminService.rpc.service_base_port = $new_port_grapher_datastore |
            .chrono_grapher.Monitoring.monitor.file = ($monitor_dir + "/" + ($grapher_index | tostring) + "_" + $grapher_monitoring_file_name) |
            .chrono_grapher.Extractors.story_files_dir = ($output_dir + "/")' "$default_conf" > "$grapher_output_file"

        echo "Generated $grapher_output_file with ports $new_port_grapher_drain and $new_port_grapher_datastore"
    done

    # Generate player configuration files
    echo "Generating player configuration files ..."
    for (( i=0; i<num_recording_groups; i++ )); do
        local new_port_player_datastore=$((base_port_player_datastore + i))
        local new_port_player_playback=$((base_port_player_playback + i))

        local player_index=$((i + 1))
        local player_output_file="${conf_dir}/player_conf_${player_index}.json"

        player_monitoring_file=$(jq -r '.chrono_player.Monitoring.monitor.file' "$default_conf")
        player_monitoring_file_name=$(basename "$player_monitoring_file")
        jq --arg monitor_dir "$monitor_dir" \
            --argjson new_port_player_datastore $new_port_player_datastore \
            --argjson new_port_player_playback $new_port_player_playback \
            --argjson player_index "$player_index" \
            --arg player_monitoring_file_name "$player_monitoring_file_name" \
           '.chrono_player.RecordingGroup = $player_index |
            .chrono_player.PlayerStoreAdminService.rpc.service_base_port = $new_port_player_datastore |
            .chrono_player.PlaybackQueryService.rpc.service_base_port = $new_port_player_playback |
            .chrono_player.Monitoring.monitor.file = ($monitor_dir + "/" + ($player_index | tostring) + "_" + $player_monitoring_file_name)' "$default_conf" > "$player_output_file"

        echo "Generated $player_output_file with port $new_port_player_datastore"
    done

    # Assign keepers to graphers iteratively
    echo "Generating keeper configuration files ..."
    for (( i=0; i<num_keepers; i++ )); do
        local new_port_keeper_record=$((base_port_keeper_record + i))
        local new_port_keeper_datastore=$((base_port_keeper_datastore + i))
        local grapher_index=$((i % num_recording_groups + 1))
        local new_port_keeper_drain=$((base_port_keeper_drain + grapher_index - 1))

        local keeper_index=$((i + 1))
        local keeper_output_file="${conf_dir}/keeper_conf_${keeper_index}.json"

        keeper_monitoring_file=$(jq -r '.chrono_keeper.Monitoring.monitor.file' "$default_conf")
        keeper_monitoring_file_name=$(basename "$keeper_monitoring_file")
        jq --arg monitor_dir "$monitor_dir" \
            --arg output_dir "$output_dir" \
            --argjson new_port_keeper_record $new_port_keeper_record \
            --argjson new_port_keeper_drain $new_port_keeper_drain \
            --argjson new_port_keeper_datastore $new_port_keeper_datastore \
            --argjson grapher_index "$grapher_index" \
            --arg keeper_index "$keeper_index" \
            --arg keeper_monitoring_file_name "$keeper_monitoring_file_name" \
            '.chrono_keeper.KeeperRecordingService.rpc.service_base_port = $new_port_keeper_record |
            .chrono_keeper.KeeperGrapherDrainService.rpc.service_base_port = $new_port_keeper_drain |
            .chrono_keeper.KeeperDataStoreAdminService.rpc.service_base_port = $new_port_keeper_datastore |
            .chrono_keeper.story_files_dir = ($output_dir + "/") |
            .chrono_keeper.RecordingGroup = $grapher_index |
            .chrono_keeper.Monitoring.monitor.file = ($monitor_dir + "/" + ($keeper_index | tostring) + "_" + $keeper_monitoring_file_name)' "$default_conf" > "$keeper_output_file"
        echo "Generated $keeper_output_file with ports $new_port_keeper_record, $new_port_keeper_datastore, and $new_port_keeper_drain"
    done

    # Generate visor configuration file
    local visor_output_file="${conf_dir}/visor_conf.json"
    visor_monitoring_file=$(jq -r '.chrono_visor.Monitoring.monitor.file' "$default_conf")
    visor_monitoring_file_name=$(basename "$visor_monitoring_file")
    jq --arg monitor_dir "$monitor_dir" \
        --arg visor_monitoring_file_name "$visor_monitoring_file_name" \
       '.chrono_visor.Monitoring.monitor.file = ($monitor_dir + "/" + $visor_monitoring_file_name)' "$default_conf" > "$visor_output_file"
    echo "Generated $visor_output_file"

    echo "Generate configuration files for all recording groups done"
}

check_dependencies() {
    local dependencies=("jq" "ldd" "nohup" "pkill" "readlink")
    echo -e "${DEBUG}Checking required dependencies...${NC}"
    for dep in "${dependencies[@]}"; do
        if ! command -v $dep &> /dev/null; then
            echo -e "${ERR}Dependency $dep is not installed. Please install it and try again.${NC}"
            exit 1
        fi
    done
    echo -e "${DEBUG}All required dependencies are installed.${NC}"
}

check_directories() {
    echo -e "${DEBUG}Checking required directories...${NC}"
    local directories=("${WORK_DIR}" "${LIB_DIR}" "${CONF_DIR}" "${BIN_DIR}")

    for dir in "${directories[@]}"; do
        if [[ ! -d ${dir} ]]; then
            echo -e "${ERR}Directory ${dir} does not exist. Please create it and try again.${NC}"
            exit 1
        fi
    done
    echo -e "${DEBUG}All required directories are in place.${NC}"
}

check_files() {
    echo -e "${DEBUG}Checking required files...${NC}"
    [[ ! -f ${VISOR_BIN} ]] && echo -e "${ERR}Visor binary file does not exist, exiting ...${NC}" && exit 1
    [[ ! -f ${KEEPER_BIN} ]] && echo -e "${ERR}Keeper binary file does not exist, exiting ...${NC}" && exit 1
    [[ ! -f ${GRAPHER_BIN} ]] && echo -e "${ERR}Grapher binary file does not exist, exiting ...${NC}" && exit 1
    [[ ! -f ${PLAYER_BIN} ]] && echo -e "${ERR}Player binary file does not exist, exiting ...${NC}" && exit 1
    [[ ! -f ${CONF_FILE} ]] && echo -e "${ERR}Configuration file does not exist, exiting ...${NC}" && exit 1
    echo -e "${DEBUG}All required files are in place.${NC}"
}

check_build_directory() {
    local deploy_dir=$(realpath "$(dirname "$0")")  # Get the absolute path of the script's directory
    local build_dir="${deploy_dir}/../build"       # Navigate to x/build from x/deploy

    build_dir=$(realpath "${build_dir}" 2>/dev/null || echo "")

    echo -e "${DEBUG}Checking for the existence of the build directory: ${build_dir}${NC}"

    if [ -d "${build_dir}" ]; then
        echo -e "${DEBUG}Build directory found: ${build_dir}${NC}"
    else
        echo -e "${ERR}Build directory not found at: ${build_dir}${NC}"
        echo "Please ensure the build process has been completed successfully before proceeding."
        exit 1
    fi
}

check_installation() {
    check_dependencies
    check_directories
    check_files
}

check_work_dir() {
    # Check if WORK_DIR is set
    if [[ -z "${WORK_DIR}" ]]; then
        echo -e "${ERR}WORK_DIR is mandatory on this mode. Please provide it using the -w or --work-dir option.${NC}"
        usage
        exit 1
    fi
}

check_execution_stopped() {
    echo -e "${DEBUG}Checking if ChronoLog processes are running...${NC}"
    local active_processes=$(pgrep -la chrono)

    if [[ -n "${active_processes}" ]]; then
        echo -e "${ERR}ChronoLog processes are still running:${NC}"
        echo "${active_processes}"
        echo -e "${ERR}Please stop all ChronoLog processes before cleaning.${NC}"
        exit 1
    else
        echo -e "${DEBUG}No active ChronoLog processes detected. Proceeding with cleaning.${NC}"
    fi
}

# Main functions __________________________________________________________________________________________________________
build() {
    if [[ -n "$INSTALL_DIR" ]]; then
        "${REPO_ROOT}/deploy/build.sh" -type "$BUILD_TYPE" -install-path "$INSTALL_DIR"
    else
        "${REPO_ROOT}/deploy/build.sh" -type "$BUILD_TYPE"
    fi
}

install() {
    check_work_dir
    check_build_directory
    install_script="${REPO_ROOT}/deploy/install.sh"
    if [[ -x "$install_script" ]]; then
        "$install_script" --lib-dir "${LIB_DIR}" --bin-dir "${BIN_DIR}"
    else
        echo -e "${RED}Error: $install_script is not executable or not found.${NC}"
        exit 1
    fi
}

start() {
    echo -e "${INFO}Preparing to start ChronoLog...${NC}"
    check_work_dir
    mkdir -p "${MONITOR_DIR}"
    mkdir -p "${OUTPUT_DIR}"
    check_installation
    generate_config_files ${NUM_KEEPERS} ${CONF_FILE} ${CONF_DIR} ${OUTPUT_DIR} ${NUM_RECORDING_GROUPS} ${MONITOR_DIR}
    echo -e "${INFO}Starting ChronoLog...${NC}"
    start_service ${VISOR_BIN} "--config ${CONF_DIR}/visor_conf.json" "visor.launch.log"
    sleep 2
    num_record_group=${NUM_RECORDING_GROUPS}
    for (( i=1; i<=num_record_group; i++ ))
    do
        start_service ${GRAPHER_BIN} "--config ${CONF_DIR}/grapher_conf_$i.json" "grapher_$i.launch.log"
    done
    sleep 2
    for (( i=1; i<=num_record_group; i++ ))
    do
        start_service ${PLAYER_BIN} "--config ${CONF_DIR}/player_conf_$i.json" "player_$i.launch.log"
    done
    sleep 2
    num_keepers=${NUM_KEEPERS}
    for (( i=1; i<=num_keepers; i++ ))
    do
        start_service ${KEEPER_BIN} "--config ${CONF_DIR}/keeper_conf_$i.json" "keeper_$i.launch.log"
    done
    echo -e "${INFO}ChronoLog Started.${NC}"
}

stop() {
    echo -e "${INFO}Stopping ChronoLog...${NC}"
    check_work_dir
    stop_service ${PLAYER_BIN} 100
    stop_service ${KEEPER_BIN} 100
    stop_service ${GRAPHER_BIN} 100
    stop_service ${VISOR_BIN} 100
    echo -e "${INFO}ChronoLog stopped.${NC}"
}

clean() {
    echo -e "${INFO}Cleaning ChronoLog...${NC}"
    check_work_dir
    check_execution_stopped
    echo -e "${DEBUG}Removing config files${NC}"
    rm -f ${CONF_DIR}/grapher_conf*.json
    rm -f ${CONF_DIR}/player_conf*.json
    rm -f ${CONF_DIR}/keeper_conf*.json
    rm -f ${CONF_DIR}/visor_conf.json

    echo -e "${DEBUG}Removing log files${NC}"
    rm -f ${MONITOR_DIR}/*.log

    echo -e "${DEBUG}Removing output files${NC}"
    rm -f ${OUTPUT_DIR}/*

    echo -e "${INFO}ChronoLog cleaning done. ${NC}"
}

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
    echo "  -c|--clean          Clean ChronoLog logging artifacts (default: false)"
    echo "  Note: Only one execution mode can be selected per run."
    echo ""
    echo "Build Options:"
    echo "  -t|--build-type <Debug|Release>  Define type of build (default: Release) [Modes: Build]"
    echo "  -l|--install-dir <path>          Define installation directory (default: /home/$USER/chronolog/BUILD_TYPE) [Modes: Build]"
    echo ""
    echo "Deployment Options:"
    echo "  -k|--keepers <number>            Set the total number of keeper processes. They will be assigned iteratively to the recording groups (default: 1)[Modes: Start]"
    echo "  -r|--record-groups <number>      Set the number of recording groups or grapher processes (default: 1)[Modes: Start]"
    echo ""
    echo "Directory Settings:"
    echo "  -w|--work-dir <path>             Set the working directory (Mandatory) [Modes: Install, Start, Stop, Clean]"
    echo "  -m|--monitor-dir <path>          Set the monitor directory (default: work_dir/monitor) [Modes: Start]"
    echo "  -u|--output-dir <path>           Set the output directory (default: work_dir/output) [Modes: Start]"
    echo ""
    echo "Binary Paths:"
    echo "  -v|--visor-bin <path>            Path to the ChronoVisor binary (default: work_dir/bin/chronovisor_server) [Modes: Start]"
    echo "  -g|--grapher-bin <path>          Path to the ChronoGrapher binary (default: work_dir/bin/chrono_grapher) [Modes: Start]"
    echo "  -p|--keeper-bin <path>           Path to the ChronoKeeper binary (default: work_dir/bin/chrono_keeper) [Modes: Start]"
    echo "  -a|--player-bin <path>           Path to the ChronoPlayer binary (default: work_dir/bin/chrono_player) [Modes: Start]"
    echo ""
    echo "Configuration Settings:"
    echo "  -f|--conf-file <path>            Path to the configuration file (default: work_dir/conf/default_conf.json) [Modes: Start]"
    echo ""
    echo "Examples (Assume installing a Debug build to ~/chronolog_install):"
    echo ""
    echo "  1) Builds ChronoLog in Debug mode and installs it to ~/chronolog_install:"
    echo "     $0 --build --build-type Debug --install-dir ~/chronolog_install"
    echo ""
    echo "  2) Installs ChronoLog to the working directory (~/chronolog_install/Debug):"
    echo "     $0 --install --work-dir ~/chronolog_install/Debug"
    echo ""
    echo "  3) Starts a ChronoLog deployment using the default configuration and paths,"
    echo "     with binaries from the working directory ~/chronolog_install/Debug:"
    echo "     $0 --start --work-dir ~/chronolog_install/Debug"
    echo ""
    echo "  4) Starts a ChronoLog deployment with 5 keeper processes and 2 grapher processes,"
    echo "     using the working directory ~/chronolog_install/Debug:"
    echo "     $0 -d -k 5 -r 2 --work-dir ~/chronolog_install/Debug"
    echo ""
    echo "  5) Stops the ChronoLog deployment using the working directory ~/chronolog_install/Debug:"
    echo "     $0 -s --work-dir ~/chronolog_install/Debug"
    echo ""
    exit 1
}


parse_args() {
    while [[ "$#" -gt 0 ]]; do
        case "$1" in
             -h|--help)
                usage;
                shift ;;
            -b|--build)
                build=true
                ((EXEC_MODE_COUNT++))
                shift ;;
            -i|--install)
                install=true
                ((EXEC_MODE_COUNT++))
                shift ;;
            -d|--start)
                start=true
                ((EXEC_MODE_COUNT++))
                shift ;;
            -s|--stop)
                stop=true
                ((EXEC_MODE_COUNT++))
                shift ;;
            -c|--clean)
                clean=true
                ((EXEC_MODE_COUNT++))
                shift ;;
            -t|--build-type)
                BUILD_TYPE="$2"
                if [[ "$BUILD_TYPE" != "Debug" && "$BUILD_TYPE" != "Release" ]]; then
                    echo -e "${ERR}Invalid build type: $BUILD_TYPE. Must be 'Debug' or 'Release'.${NC}"
                    usage
                fi
                shift 2 ;;
            -l|--install-dir)
                INSTALL_DIR=$(realpath "$2")
                shift 2 ;;
            -k|--keepers)
                NUM_KEEPERS="$2"
                shift 2 ;;
            -r|--record-groups)
                NUM_RECORDING_GROUPS="$2"
                shift 2 ;;
            -w|--work-dir)
                WORK_DIR="${2%/}"
                LIB_DIR="${WORK_DIR}/lib"
                CONF_DIR="${WORK_DIR}/conf"
                BIN_DIR="${WORK_DIR}/bin"
                VISOR_BIN="${BIN_DIR}/chronovisor_server"
                KEEPER_BIN="${BIN_DIR}/chrono_keeper"
                GRAPHER_BIN="${BIN_DIR}/chrono_grapher"
                PLAYER_BIN="${BIN_DIR}/chrono_player"
                CONF_FILE="${CONF_DIR}/default_conf.json"
                OUTPUT_DIR=${WORK_DIR}/output
                MONITOR_DIR=${WORK_DIR}/monitor
                shift 2 ;;
            -u|--output-dir)
                OUTPUT_DIR=$(realpath "$2")
                shift 2 ;;
            -m|--monitor-dir)
                MONITOR_DIR=$(realpath "$2")
                shift 2 ;;
            -v|--visor-bin)
                VISOR_BIN=$(realpath "$2")
                shift 2 ;;
            -g|--grapher-bin)
                GRAPHER_BIN=$(realpath "$2")
                shift 2 ;;
            -p|--keeper-bin)
                KEEPER_BIN=$(realpath "$2")
                shift 2 ;;
            -a|--player-bin)
                PLAYER_BIN=$(realpath "$2")
                shift 2 ;;
            -f|--conf-file)
                CONF_FILE=$(realpath "$2")
                CONF_DIR=$(dirname ${CONF_FILE})
                shift 2 ;;
            *) echo -e "${ERR}Unknown option: $1${NC}"; usage ;;
        esac
    done

    # Check if multiple execution modes are selected
    if [[ $EXEC_MODE_COUNT -gt 1 ]]; then
        echo -e "${ERR}Error: Only one execution mode can be selected at a time. Please choose one of build, install, start, stop, or clean.${NC}"
        usage
        exit 1
    elif [[ $EXEC_MODE_COUNT -eq 0 ]]; then
        echo -e "${ERR}Error: Please specify an execution mode (build, install, start, stop, or clean).${NC}"
        usage
        exit 1
    fi
}

# Start execution of the script____________________________________________________________________________________
parse_args "$@"

if ${build}
then
    build
elif ${install}
then
    install
elif ${start}
then
    start
elif ${stop}
then
    stop
elif ${clean}
then
    clean
else
    echo -e "${ERR}Please select build, install, start, stop or clean mode${NC}"
    usage
fi