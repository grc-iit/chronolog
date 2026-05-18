#!/bin/bash

# Deployment helper that runs directly from an installed ChronoLog tree.
# Supports starting, stopping, and cleaning a local single-user deployment
# without requiring the source repository.

set -e

# Colors
ERR='\033[7;37m\033[41m'
INFO='\033[7;49m\033[92m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

# Defaults
NUM_KEEPERS=1
NUM_RECORDING_GROUPS=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_WORK_DIR="$(realpath -m "${SCRIPT_DIR}/..")"
DEFAULT_INSTALL_WORK_DIR="$HOME/chronolog-install/chronolog"
WORK_DIR_USER_SET=false
OUTPUT_DIR_USER_SET=false
MONITOR_DIR_USER_SET=false

# Directories (initialize with defaults; may be overridden via CLI)
WORK_DIR="${DEFAULT_WORK_DIR}"
LIB_DIR="${WORK_DIR}/lib"
CONF_DIR="${WORK_DIR}/conf"
BIN_DIR="${WORK_DIR}/bin"
MONITOR_DIR="${WORK_DIR}/monitor"
OUTPUT_DIR="${WORK_DIR}/output"

# Files (with defaults)
VISOR_BIN="${BIN_DIR}/chrono-visor"
KEEPER_BIN="${BIN_DIR}/chrono-keeper"
GRAPHER_BIN="${BIN_DIR}/chrono-grapher"
PLAYER_BIN="${BIN_DIR}/chrono-player"
CONF_FILE="${CONF_DIR}/default-chrono-conf.json"
CLIENT_CONF_FILE="${CONF_DIR}/default-chrono-client-conf.json"

# Flags
start=false
stop=false
clean=false
EXEC_MODE_COUNT=0

# Helper Methods -----------------------------------------------------------------------------------
start_service() {
    local bin="$1"
    local args="$2"
    local monitor_file="$3"
    echo -e "${DEBUG}Launching $bin $args ...${NC}"
    nohup ${bin} ${args} > "${MONITOR_DIR}/${monitor_file}" 2>&1 &
}

kill_service() {
    local bin="$1"
    local name
    name=$(basename "$bin")
    echo -e "${DEBUG}Killing ${name} ...${NC}"
    pkill -9 -f "${bin}"
}

stop_service() {
    local bin="$1"
    local name
    name=$(basename "$bin")
    local timeout="$2"

    local start_time
    start_time=$(date +%s)
    echo -e "${DEBUG}Stopping ${name} ...${NC}"
    pkill -f "${bin}" || true

    while true; do
        if pgrep -f "${bin}" >/dev/null; then
            echo -e "${DEBUG}Waiting for ${name} to stop...${NC}"
        else
            echo -e "${DEBUG}All ${name} processes stopped gracefully.${NC}"
            break
        fi
        sleep 10
        local current_time
        current_time=$(date +%s)
        if (( current_time - start_time >= timeout )); then
            echo -e "${DEBUG}Timeout reached while stopping ${name}. Forcing termination.${NC}"
            if pgrep -f "${bin}" >/dev/null; then
                kill_service "${bin}"
                echo -e "${DEBUG}Killed: ${name} ${NC}"
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
    local client_conf_file=$7

    mkdir -p "${monitor_dir}"

    # Check if default configuration files exist
    if [ ! -f "$default_conf" ]; then
        echo "Default configuration file $default_conf not found."
        exit 1
    fi

    if [ ! -f "$client_conf_file" ]; then
        echo "Default configuration file $client_conf_file not found."
        exit 1
    fi

    # Validate counts
    if (( num_keepers <= 0 || num_recording_groups <= 0 )); then
        echo "Number of keepers and recording groups must be greater than 0. Exiting..."
        exit 1
    fi

    if (( num_keepers < num_recording_groups )); then
        echo "Number of keepers must be greater than or equal to the number of recording groups. Exiting..."
        exit 1
    fi

    # Extract base ports
    local base_port_keeper_record base_port_keeper_drain base_port_keeper_datastore
    local base_port_grapher_drain base_port_grapher_datastore
    local base_port_player_datastore base_port_player_playback
    base_port_keeper_record=$(jq -r '.chrono_keeper.KeeperRecordingService.rpc.service_base_port' "$default_conf")
    base_port_keeper_drain=$(jq -r '.chrono_keeper.KeeperGrapherDrainService.rpc.service_base_port' "$default_conf")
    base_port_keeper_datastore=$(jq -r '.chrono_keeper.KeeperDataStoreAdminService.rpc.service_base_port' "$default_conf")
    base_port_grapher_drain=$(jq -r '.chrono_grapher.KeeperGrapherDrainService.rpc.service_base_port' "$default_conf")
    base_port_grapher_datastore=$(jq -r '.chrono_grapher.DataStoreAdminService.rpc.service_base_port' "$default_conf")
    base_port_player_datastore=$(jq -r '.chrono_player.PlayerStoreAdminService.rpc.service_base_port' "$default_conf")
    base_port_player_playback=$(jq -r '.chrono_player.PlaybackQueryService.rpc.service_base_port' "$default_conf")

    echo "Generating grapher configuration files ..."
    mkdir -p "${output_dir}"

    for (( i=0; i<num_recording_groups; i++ )); do
        local new_port_grapher_drain=$((base_port_grapher_drain + i))
        local new_port_grapher_datastore=$((base_port_grapher_datastore + i))

        local grapher_index=$((i + 1))
        local grapher_output_file="${conf_dir}/chrono-grapher-conf-${grapher_index}.json"

        jq --arg monitor_dir "$monitor_dir" \
            --arg output_dir "$output_dir" \
            --argjson new_port_grapher_drain $new_port_grapher_drain \
            --argjson new_port_grapher_datastore $new_port_grapher_datastore \
            --argjson grapher_index "$grapher_index" \
           '.chrono_grapher.RecordingGroup = $grapher_index |
            .chrono_grapher.KeeperGrapherDrainService.rpc.service_base_port = $new_port_grapher_drain |
            .chrono_grapher.DataStoreAdminService.rpc.service_base_port = $new_port_grapher_datastore |
            .chrono_grapher.Monitoring.monitor.file = ($monitor_dir + "/chrono-grapher-" + ($grapher_index | tostring) + ".log") |
            .chrono_grapher.ExtractionModule.extractors.hdf5_archive_extractor.hdf5_archive_dir = ($output_dir + "/")' "$default_conf" > "$grapher_output_file"

        echo "Generated $grapher_output_file with ports $new_port_grapher_drain and $new_port_grapher_datastore"
    done

    echo "Generating player configuration files ..."
    for (( i=0; i<num_recording_groups; i++ )); do
        local new_port_player_datastore=$((base_port_player_datastore + i))
        local new_port_player_playback=$((base_port_player_playback + i))

        local player_index=$((i + 1))
        local player_output_file="${conf_dir}/chrono-player-conf-${player_index}.json"

        jq --arg monitor_dir "$monitor_dir" \
            --arg output_dir "$output_dir" \
            --argjson new_port_player_datastore $new_port_player_datastore \
            --argjson new_port_player_playback $new_port_player_playback \
            --argjson player_index "$player_index" \
           '.chrono_player.RecordingGroup = $player_index |
            .chrono_player.PlayerStoreAdminService.rpc.service_base_port = $new_port_player_datastore |
            .chrono_player.PlaybackQueryService.rpc.service_base_port = $new_port_player_playback |
            .chrono_player.Monitoring.monitor.file = ($monitor_dir + "/chrono-player-" + ($player_index | tostring) + ".log") |
            .chrono_player.ArchiveReaders.story_files_dir = ($output_dir + "/")' "$default_conf" >"$player_output_file"

        echo "Generated $player_output_file with port $new_port_player_datastore"
    done

    echo "Generating keeper configuration files ..."
    for (( i=0; i<num_keepers; i++ )); do
        local new_port_keeper_record=$((base_port_keeper_record + i))
        local new_port_keeper_datastore=$((base_port_keeper_datastore + i))
        local grapher_index=$((i % num_recording_groups + 1))
        local new_port_keeper_drain=$((base_port_keeper_drain + grapher_index - 1))

        local keeper_index=$((i + 1))
        local keeper_output_file="${conf_dir}/chrono-keeper-conf-${keeper_index}.json"

        jq --arg monitor_dir "$monitor_dir" \
            --arg output_dir "$output_dir" \
            --argjson new_port_keeper_record $new_port_keeper_record \
            --argjson new_port_keeper_drain $new_port_keeper_drain \
            --argjson new_port_keeper_datastore $new_port_keeper_datastore \
            --argjson grapher_index "$grapher_index" \
            --arg keeper_index "$keeper_index" \
            '.chrono_keeper.KeeperRecordingService.rpc.service_base_port = $new_port_keeper_record |
            .chrono_keeper.KeeperDataStoreAdminService.rpc.service_base_port = $new_port_keeper_datastore |
            .chrono_keeper.ExtractionModule.extractors.csv_tier_extractor.csv_archive_dir = ($output_dir + "/") |
            .chrono_keeper.ExtractionModule.extractors.extractor_to_grapher.receiving_endpoint.service_base_port = $new_port_keeper_drain |
            .chrono_keeper.RecordingGroup = $grapher_index |
            .chrono_keeper.Monitoring.monitor.file = ($monitor_dir + "/chrono-keeper-" + ($keeper_index | tostring) + ".log")' "$default_conf" > "$keeper_output_file"
        echo "Generated $keeper_output_file with ports $new_port_keeper_record, $new_port_keeper_datastore, and $new_port_keeper_drain"
    done

    echo "Generating visor configuration file ..."
    local visor_output_file="${conf_dir}/chrono-visor-conf.json"
    visor_monitoring_file_name=$(jq -r '.chrono_visor.Monitoring.monitor.file | split("/") | last' "$default_conf")
    jq --arg monitor_dir "$monitor_dir" \
        --arg visor_monitoring_file_name "$visor_monitoring_file_name" \
       '.chrono_visor.Monitoring.monitor.file = ($monitor_dir + "/" + $visor_monitoring_file_name)' "$default_conf" > "$visor_output_file"
    echo "Generated $visor_output_file"

    echo "Generating client configuration file ..."
    local client_output_file="${conf_dir}/chrono-client-conf.json"
    client_monitoring_file_name=$(jq -r '.chrono_client.Monitoring.monitor.file | split("/") | last' "$client_conf_file")
    jq --arg monitor_dir "$monitor_dir" \
        --arg client_monitoring_file_name "$client_monitoring_file_name" \
       '.chrono_client.Monitoring.monitor.file = ($monitor_dir + "/" + $client_monitoring_file_name)' "$client_conf_file" > "$client_output_file"
    echo "Generated $client_output_file"

    echo "Generate configuration files for all recording groups done"
}

check_dependencies() {
    local dependencies=("jq" "ldd" "nohup" "pkill" "readlink" "realpath" "chrpath")
    echo -e "${DEBUG}Checking required dependencies...${NC}"
    for dep in "${dependencies[@]}"; do
        if ! command -v "$dep" &> /dev/null; then
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
    [[ ! -f ${CLIENT_CONF_FILE} ]] && echo -e "${ERR}Client configuration file does not exist, exiting ...${NC}" && exit 1
    echo -e "${DEBUG}All required files are in place.${NC}"
}

check_installation() {
    check_dependencies
    check_directories
    check_files
}

refresh_paths_for_work_dir() {
    LIB_DIR="${WORK_DIR}/lib"
    CONF_DIR="${WORK_DIR}/conf"
    BIN_DIR="${WORK_DIR}/bin"
    # Preserve user-set --output-dir / --monitor-dir; only re-derive from WORK_DIR
    # when the user didn't override them explicitly.
    if [[ "${MONITOR_DIR_USER_SET}" == "false" ]]; then
        MONITOR_DIR="${WORK_DIR}/monitor"
    fi
    if [[ "${OUTPUT_DIR_USER_SET}" == "false" ]]; then
        OUTPUT_DIR="${WORK_DIR}/output"
    fi
    VISOR_BIN="${BIN_DIR}/chrono-visor"
    KEEPER_BIN="${BIN_DIR}/chrono-keeper"
    GRAPHER_BIN="${BIN_DIR}/chrono-grapher"
    PLAYER_BIN="${BIN_DIR}/chrono-player"
    CONF_FILE="${CONF_DIR}/default-chrono-conf.json"
    CLIENT_CONF_FILE="${CONF_DIR}/default-chrono-client-conf.json"
}

check_work_dir() {
    # Only attempt auto-detection if the user did not set --work-dir
    if [[ -z "${WORK_DIR}" ]]; then
        WORK_DIR="${DEFAULT_WORK_DIR}"
    fi

    # If the default work dir looks empty (no bin), try the typical install location
    if [[ "${WORK_DIR_USER_SET}" == "false" && ! -d "${WORK_DIR}/bin" ]]; then
        if [[ -d "${DEFAULT_INSTALL_WORK_DIR}/bin" ]]; then
            echo -e "${DEBUG}Default work dir missing binaries; using installed tree at ${DEFAULT_INSTALL_WORK_DIR}${NC}"
            WORK_DIR="${DEFAULT_INSTALL_WORK_DIR}"
        fi
    fi

    refresh_paths_for_work_dir

    if [[ ! -d "${WORK_DIR}/bin" ]]; then
        echo -e "${ERR}Work dir '${WORK_DIR}' does not look like an installed ChronoLog tree (missing bin/). Use --work-dir to point to your installation or run install.sh.${NC}"
        exit 1
    fi
}

check_execution_stopped() {
    echo -e "${DEBUG}Checking if ChronoLog processes are running...${NC}"
    local active_processes
    active_processes=$(pgrep -la chrono || true)

    if [[ -n "${active_processes}" ]]; then
        echo -e "${ERR}ChronoLog processes are still running:${NC}"
        echo "${active_processes}"
        echo -e "${ERR}Please stop all ChronoLog processes before cleaning.${NC}"
        exit 1
    else
        echo -e "${DEBUG}No active ChronoLog processes detected. Proceeding with cleaning.${NC}"
    fi
}

# Main functions ------------------------------------------------------------------------------------
start() {
    echo -e "${INFO}Preparing to start ChronoLog...${NC}"
    check_work_dir
    mkdir -p "${MONITOR_DIR}" "${OUTPUT_DIR}"
    check_installation
    generate_config_files "${NUM_KEEPERS}" "${CONF_FILE}" "${CONF_DIR}" "${OUTPUT_DIR}" "${NUM_RECORDING_GROUPS}" "${MONITOR_DIR}" "${CLIENT_CONF_FILE}"
    echo -e "${INFO}Starting ChronoLog...${NC}"
    start_service "${VISOR_BIN}" "--config ${CONF_DIR}/chrono-visor-conf.json" "chrono-visor.launch.log"
    sleep 2
    for (( i=1; i<=NUM_RECORDING_GROUPS; i++ )); do
        start_service "${GRAPHER_BIN}" "--config ${CONF_DIR}/chrono-grapher-conf-$i.json" "chrono-grapher-$i.launch.log"
    done
    sleep 2
    for (( i=1; i<=NUM_RECORDING_GROUPS; i++ )); do
        start_service "${PLAYER_BIN}" "--config ${CONF_DIR}/chrono-player-conf-$i.json" "chrono-player-$i.launch.log"
    done
    sleep 2
    for (( i=1; i<=NUM_KEEPERS; i++ )); do
        start_service "${KEEPER_BIN}" "--config ${CONF_DIR}/chrono-keeper-conf-$i.json" "chrono-keeper-$i.launch.log"
    done
    echo -e "${INFO}ChronoLog Started.${NC}"
}

stop() {
    echo -e "${INFO}Stopping ChronoLog...${NC}"
    check_work_dir
    stop_service "${PLAYER_BIN}" 100
    stop_service "${KEEPER_BIN}" 100
    stop_service "${GRAPHER_BIN}" 100
    stop_service "${VISOR_BIN}" 100
    echo -e "${INFO}All ChronoLog processes stopped.${NC}"
}

clean() {
    echo -e "${INFO}Cleaning ChronoLog...${NC}"
    check_work_dir
    check_execution_stopped
    echo -e "${DEBUG}Removing config files${NC}"
    rm -f "${CONF_DIR}/chrono-grapher-conf"*.json
    rm -f "${CONF_DIR}/chrono-player-conf"*.json
    rm -f "${CONF_DIR}/chrono-keeper-conf"*.json
    rm -f "${CONF_DIR}/chrono-visor-conf.json"
    rm -f "${CONF_DIR}/chrono-client-conf.json"

    echo -e "${DEBUG}Removing log files${NC}"
    rm -f "${MONITOR_DIR}"/*.log

    echo -e "${DEBUG}Removing output files${NC}"
    rm -f "${OUTPUT_DIR}"/*

    echo -e "${INFO}ChronoLog cleaning done.${NC}"
}

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Execution Modes (Select only ONE):"
    echo "  -d|--start          Start ChronoLog Deployment"
    echo "  -s|--stop           Stop ChronoLog Deployment"
    echo "  -c|--clean          Clean ChronoLog logging artifacts"
    echo ""
    echo "Deployment Options:"
    echo "  -k|--keepers <number>            Total number of keeper processes (default: 1)"
    echo "  -r|--record-groups <number>      Number of recording groups/graphers (default: 1)"
    echo ""
    echo "Directory Settings:"
    echo "  -w|--work-dir <path>             Working directory (default: script_dir/..)"
    echo "  -m|--monitor-dir <path>          Monitor directory (default: work_dir/monitor)"
    echo "  -u|--output-dir <path>           Output directory (default: work_dir/output)"
    echo ""
    echo "Binary Paths:"
    echo "  -v|--visor-bin <path>            Path to ChronoVisor binary (default: work_dir/bin/chrono-visor)"
    echo "  -g|--grapher-bin <path>          Path to ChronoGrapher binary (default: work_dir/bin/chrono-grapher)"
    echo "  -p|--keeper-bin <path>           Path to ChronoKeeper binary (default: work_dir/bin/chrono-keeper)"
    echo "  -a|--player-bin <path>           Path to ChronoPlayer binary (default: work_dir/bin/chrono-player)"
    echo ""
    echo "Configuration Settings:"
    echo "  -f|--conf-file <path>            Path to configuration file (default: work_dir/conf/default-chrono-conf.json)"
    echo "  -n|--client-conf-file <path>     Path to client configuration file (default: work_dir/conf/default-chrono-client-conf.json)"
    echo ""
    echo "Examples:"
    echo "  Start with defaults from install tree:"
    echo "    $0 --start"
    echo ""
    echo "  Start with 5 keepers and 2 recording groups:"
    echo "    $0 --start --keepers 5 --record-groups 2"
    echo ""
    exit 1
}

parse_args() {
    while [[ "$#" -gt 0 ]]; do
        case "$1" in
            -h|--help)
                usage;
                shift ;;
            -d|--start)
                start=true
                ((++EXEC_MODE_COUNT))
                shift ;;
            -s|--stop)
                stop=true
                ((++EXEC_MODE_COUNT))
                shift ;;
            -c|--clean)
                clean=true
                ((++EXEC_MODE_COUNT))
                shift ;;
            -k|--keepers)
                NUM_KEEPERS="$2"
                shift 2 ;;
            -r|--record-groups)
                NUM_RECORDING_GROUPS="$2"
                shift 2 ;;
            -w|--work-dir)
                WORK_DIR="${2%/}"
                WORK_DIR_USER_SET=true
                # Path derivation happens in refresh_paths_for_work_dir() after
                # arg parsing completes, so --output-dir / --monitor-dir are
                # honored regardless of argv order.
                shift 2 ;;
            -u|--output-dir)
                OUTPUT_DIR=$(realpath -m "$2")
                OUTPUT_DIR_USER_SET=true
                shift 2 ;;
            -m|--monitor-dir)
                MONITOR_DIR=$(realpath -m "$2")
                MONITOR_DIR_USER_SET=true
                shift 2 ;;
            -v|--visor-bin)
                VISOR_BIN=$(realpath -m "$2")
                shift 2 ;;
            -g|--grapher-bin)
                GRAPHER_BIN=$(realpath -m "$2")
                shift 2 ;;
            -p|--keeper-bin)
                KEEPER_BIN=$(realpath -m "$2")
                shift 2 ;;
            -a|--player-bin)
                PLAYER_BIN=$(realpath -m "$2")
                shift 2 ;;
            -f|--conf-file)
                CONF_FILE=$(realpath -m "$2")
                CONF_DIR=$(dirname "${CONF_FILE}")
                shift 2 ;;
            -n|--client-conf-file)
                CLIENT_CONF_FILE=$(realpath -m "$2")
                shift 2 ;;
            *)
                echo -e "${ERR}Unknown option: $1${NC}"
                usage ;;
        esac
    done

    if [[ $EXEC_MODE_COUNT -gt 1 ]]; then
        echo -e "${ERR}Error: Only one execution mode can be selected at a time.${NC}"
        usage
    elif [[ $EXEC_MODE_COUNT -eq 0 ]]; then
        echo -e "${ERR}Error: Please specify an execution mode (start, stop, or clean).${NC}"
        usage
    fi
}

# Entry ---------------------------------------------------------------------------------------------
parse_args "$@"

if ${start}; then
    start
elif ${stop}; then
    stop
elif ${clean}; then
    clean
else
    echo -e "${ERR}Please select start, stop or clean mode${NC}"
    usage
fi
