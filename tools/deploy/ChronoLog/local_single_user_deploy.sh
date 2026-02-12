#!/bin/bash

# Local single-user deployment script for ChronoLog
# This script handles build/install operations and delegates start/stop/clean to deploy_local.sh
#
# Usage:
#   - Build/Install: Uses REPO_ROOT to access build.sh and install.sh
#   - Start/Stop/Clean: Delegates to deploy_local.sh (works from installed tree)

# Variables ____________________________________________________________________________________________________________
# Define some colors
ERR='\033[7;37m\033[41m'
INFO='\033[7;49m\033[92m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

# Script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(realpath "${SCRIPT_DIR}/../../../")"
DEPLOY_LOCAL_SCRIPT="${SCRIPT_DIR}/deploy_local.sh"

# Default values
NUM_KEEPERS=1
NUM_RECORDING_GROUPS=1
BUILD_TYPE="Release"
BUILD_DIR="$HOME/chronolog-build"
INSTALL_DIR="$HOME/chronolog-install"
WORK_DIR="$INSTALL_DIR/chronolog"

# Directories (with defaults)
LIB_DIR="$WORK_DIR/lib"
CONF_DIR="$WORK_DIR/conf"
BIN_DIR="$WORK_DIR/bin"
MONITOR_DIR="$WORK_DIR/monitor"
OUTPUT_DIR="$WORK_DIR/output"

# Files (with defaults)
VISOR_BIN="$WORK_DIR/bin/chronovisor_server"
KEEPER_BIN="$WORK_DIR/bin/chrono_keeper"
GRAPHER_BIN="$WORK_DIR/bin/chrono_grapher"
PLAYER_BIN="$WORK_DIR/bin/chrono_player"
CONF_FILE="$WORK_DIR/conf/default_conf.json"
CLIENT_CONF_FILE="$WORK_DIR/conf/default_client_conf.json"

# Booleans
build=false
install=false
start=false
stop=false
clean=false

EXEC_MODE_COUNT=0

# Main functions __________________________________________________________________________________________________________
build() {
    local build_args=("${REPO_ROOT}/tools/deploy/ChronoLog/build.sh" "-t" "$BUILD_TYPE" "-B" "$BUILD_DIR")

    if [[ -n "$INSTALL_DIR" ]]; then
        build_args+=("-I" "$INSTALL_DIR")
    fi

    echo -e "${DEBUG}Running: ${build_args[*]}${NC}"
    if [[ -x "${REPO_ROOT}/tools/deploy/ChronoLog/build.sh" ]]; then
        "${build_args[@]}"
    else
        echo -e "${ERR}Error: ${REPO_ROOT}/tools/deploy/ChronoLog/build.sh is not executable or not found.${NC}"
        exit 1
    fi
}

install() {
    local install_args=("${REPO_ROOT}/tools/deploy/ChronoLog/install.sh" "-t" "$BUILD_TYPE" "-B" "$BUILD_DIR")

    if [[ -n "$INSTALL_DIR" ]]; then
        install_args+=("-I" "$INSTALL_DIR")
    fi

    echo -e "${DEBUG}Running: ${install_args[*]}${NC}"
    if [[ -x "${REPO_ROOT}/tools/deploy/ChronoLog/install.sh" ]]; then
        "${install_args[@]}"
    else
        echo -e "${ERR}Error: ${REPO_ROOT}/tools/deploy/ChronoLog/install.sh is not executable or not found.${NC}"
        exit 1
    fi
}

# Delegate start/stop/clean operations to deploy_local.sh
delegate_to_deploy_local() {
    local mode="$1"

    if [[ ! -x "${DEPLOY_LOCAL_SCRIPT}" ]]; then
        echo -e "${ERR}Error: ${DEPLOY_LOCAL_SCRIPT} is not executable or not found.${NC}"
        exit 1
    fi

    # Build the argument list
    local args=("${mode}")

    # Pass through relevant parameters
    args+=("--work-dir" "${WORK_DIR}")
    args+=("--keepers" "${NUM_KEEPERS}")
    args+=("--record-groups" "${NUM_RECORDING_GROUPS}")

    # Pass custom paths if they differ from defaults derived from WORK_DIR
    [[ "${MONITOR_DIR}" != "${WORK_DIR}/monitor" ]] && args+=("--monitor-dir" "${MONITOR_DIR}")
    [[ "${OUTPUT_DIR}" != "${WORK_DIR}/output" ]] && args+=("--output-dir" "${OUTPUT_DIR}")
    [[ "${VISOR_BIN}" != "${WORK_DIR}/bin/chronovisor_server" ]] && args+=("--visor-bin" "${VISOR_BIN}")
    [[ "${GRAPHER_BIN}" != "${WORK_DIR}/bin/chrono_grapher" ]] && args+=("--grapher-bin" "${GRAPHER_BIN}")
    [[ "${KEEPER_BIN}" != "${WORK_DIR}/bin/chrono_keeper" ]] && args+=("--keeper-bin" "${KEEPER_BIN}")
    [[ "${PLAYER_BIN}" != "${WORK_DIR}/bin/chrono_player" ]] && args+=("--player-bin" "${PLAYER_BIN}")
    [[ "${CONF_FILE}" != "${WORK_DIR}/conf/default_conf.json" ]] && args+=("--conf-file" "${CONF_FILE}")
    [[ "${CLIENT_CONF_FILE}" != "${WORK_DIR}/conf/default_client_conf.json" ]] && args+=("--client-conf-file" "${CLIENT_CONF_FILE}")

    echo -e "${DEBUG}Delegating to: ${DEPLOY_LOCAL_SCRIPT} ${args[*]}${NC}"
    "${DEPLOY_LOCAL_SCRIPT}" "${args[@]}"
}

start() {
    delegate_to_deploy_local "--start"
}

stop() {
    delegate_to_deploy_local "--stop"
}

clean() {
    delegate_to_deploy_local "--clean"
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
    echo "Build & Install Options:"
    echo "  -t|--build-type <Debug|Release>  Define type of build (default: Release) [Modes: Build, Install]"
    echo "  -B|--build-dir <path>            Set the build directory (default: $HOME/chronolog-build/) [Modes: Build, Install]"
    echo "  -I|--install-dir <path>          Set the installation directory (default: $HOME/chronolog-install/) [Modes: Build, Install]"
    echo ""
    echo "Deployment Options:"
    echo "  -k|--keepers <number>            Set the total number of keeper processes. They will be assigned iteratively to the recording groups (default: 1)[Modes: Start]"
    echo "  -r|--record-groups <number>      Set the number of recording groups or grapher processes (default: 1)[Modes: Start]"
    echo ""
    echo "Directory Settings:"
    echo "  -w|--work-dir <path>             Set the working directory (default: $HOME/chronolog-install/chronolog) [Modes: Start, Stop, Clean]"
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
    echo "  -n|--client-conf-file <path>     Path to the client configuration file (default: work_dir/conf/default_client_conf.json) [Modes: Start]"
    echo ""
    echo "Examples (Assume installing a Debug build to ~/chronolog-install):"
    echo ""
    echo "  1) Builds ChronoLog in Debug mode and installs it to ~/chronolog-install:"
    echo "     $0 --build --build-type Debug --install-dir ~/chronolog-install"
    echo ""
    echo "  2) Installs ChronoLog to the working directory (~/chronolog-install):"
    echo "     $0 --install --install-dir ~/chronolog-install"
    echo ""
    echo "  3) Starts a ChronoLog deployment using the default configuration and paths,"
    echo "     with binaries from the working directory ~/chronolog-install:"
    echo "     $0 --start --work-dir ~/chronolog-install/chronolog"
    echo ""
    echo "  4) Starts a ChronoLog deployment with 5 keeper processes and 2 grapher processes,"
    echo "     using the working directory ~/chronolog-install/chronolog:"
    echo "     $0 --start --keepers 5 --record-groups 2 --work-dir ~/chronolog-install/chronolog"
    echo ""
    echo "  5) Stops the ChronoLog deployment using the working directory ~/chronolog-install/chronolog:"
    echo "     $0 --stop --work-dir ~/chronolog-install/chronolog"
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
            -B|--build-dir)
                BUILD_DIR=$(realpath -m "$2")
                shift 2 ;;
            -I|--install-dir)
                INSTALL_DIR=$(realpath -m "$2")
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
                CLIENT_CONF_FILE="${CONF_DIR}/default_client_conf.json"
                OUTPUT_DIR=${WORK_DIR}/output
                MONITOR_DIR=${WORK_DIR}/monitor
                shift 2 ;;
            -u|--output-dir)
                OUTPUT_DIR=$(realpath -m "$2")
                shift 2 ;;
            -m|--monitor-dir)
                MONITOR_DIR=$(realpath -m "$2")
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
                CONF_DIR=$(dirname ${CONF_FILE})
                shift 2 ;;
            -n|--client-conf-file)
                CLIENT_CONF_FILE=$(realpath -m "$2")
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
