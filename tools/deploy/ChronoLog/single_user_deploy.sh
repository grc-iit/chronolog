#!/bin/bash
# shellcheck disable=SC2086

# Cluster/Slurm deployment script for ChronoLog
# This script handles build/install operations and delegates start/stop/clean to deploy_cluster.sh
#
# Usage:
#   - Build/Install: Uses REPO_ROOT to access build.sh and install.sh
#   - Start/Stop/Clean: Delegates to deploy_cluster.sh (works from installed tree)

# Define some colors
ERR='\033[7;37m\033[41m'
INFO='\033[7;49m\033[92m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

# Basics
USER=$(whoami)

# Script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(realpath "${SCRIPT_DIR}/../../../")"
DEPLOY_CLUSTER_SCRIPT="${SCRIPT_DIR}/deploy_cluster.sh"

# Default values
BUILD_TYPE="Release"
BUILD_DIR="$HOME/chronolog-build"
INSTALL_DIR="$HOME/chronolog-install"
NUM_RECORDING_GROUP=1

# Directories (with defaults)
WORK_DIR="$INSTALL_DIR/chronolog"
LIB_DIR="$WORK_DIR/lib"
CONF_DIR="$WORK_DIR/conf"
BIN_DIR="$WORK_DIR/bin"
MONITOR_DIR="$WORK_DIR/monitor"
OUTPUT_DIR="$WORK_DIR/output"

# Binary names
VISOR_BIN_FILE_NAME="chrono-visor"
GRAPHER_BIN_FILE_NAME="chrono-grapher"
KEEPER_BIN_FILE_NAME="chrono-keeper"
PLAYER_BIN_FILE_NAME="chrono-player"

# Binary paths (with defaults)
VISOR_BIN="$BIN_DIR/$VISOR_BIN_FILE_NAME"
GRAPHER_BIN="$BIN_DIR/$GRAPHER_BIN_FILE_NAME"
KEEPER_BIN="$BIN_DIR/$KEEPER_BIN_FILE_NAME"
PLAYER_BIN="$BIN_DIR/$PLAYER_BIN_FILE_NAME"
VISOR_BIN_DIR="$BIN_DIR"
GRAPHER_BIN_DIR="$BIN_DIR"
KEEPER_BIN_DIR="$BIN_DIR"
PLAYER_BIN_DIR="$BIN_DIR"

# Configuration file and component-specific conf arguments (with defaults)
CONF_FILE="$WORK_DIR/conf/default-chrono-conf.json"
CLIENT_CONF_FILE="$WORK_DIR/conf/default-chrono-client-conf.json"
VISOR_ARGS="--config ${CONF_FILE}"
GRAPHER_ARGS="--config ${CONF_FILE}"
KEEPER_ARGS="--config ${CONF_FILE}"
PLAYER_ARGS="--config ${CONF_FILE}"

# Hosts files (with defaults)
VISOR_HOSTS="$WORK_DIR/conf/hosts_visor"
GRAPHER_HOSTS="$WORK_DIR/conf/hosts_grapher"
KEEPER_HOSTS="$WORK_DIR/conf/hosts_keeper"
PLAYER_HOSTS="$WORK_DIR/conf/hosts_player"

# Cluster specific settings
HOSTNAME_HS_NET_SUFFIX=""
JOB_ID=""

# Operation flags
build=false
install=false
start=false
stop=false
clean=false

# Verbose flag
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
  echo "Build & Install Options:"
  echo "  -t|--build-type <Debug|Release>  Define type of build (default: Release) [Modes: Build, Install]"
  echo "  -B|--build-dir <path>            Set the build directory (default: $HOME/chronolog-build/) [Modes: Build, Install]"
  echo "  -I|--install-dir <path>          Set the installation directory (default: $HOME/chronolog-install/) [Modes: Build, Install]"
  echo ""
  echo "Deployment Options:"
  echo "  -r|--record-groups <number>      Set the number of RecordingGroups or ChronoGrapher processes [Modes: Start]"
  echo "  -j|--job-id <number>             Set the Slurm job ID to get hosts from (default: \"\") [Modes: Start]"
  echo "  -q|--visor-hosts <path>          Set the hosts file for ChronoVisor (default: work_dir/conf/hosts_visor) [Modes: Start]"
  echo "  -k|--grapher-hosts <path>        Set the hosts file for ChronoGrapher (default: work_dir/conf/hosts_grapher) [Modes: Start]"
  echo "  -o|--keeper-hosts <path>         Set the hosts file for ChronoKeeper (default: work_dir/conf/hosts_keeper) [Modes: Start]"
  echo ""
  echo "Directory Settings:"
  echo "  -w|--work-dir <path>             Set the working directory (default: $HOME/chronolog-install/chronolog) [Modes: Start, Stop, Clean]"
  echo "  -m|--monitor-dir <path>          Set the monitoring directory (default: work_dir/monitor) [Modes: Start]"
  echo "  -u|--output-dir <path>           Set the output directory (default: work_dir/output) [Modes: Start]"
  echo ""
  echo "Binary Paths:"
  echo "  -v|--visor-bin <path>            Path to the ChronoVisor binary (default: work_dir/bin/chrono-visor) [Modes: Start]"
  echo "  -g|--grapher-bin <path>          Path to the ChronoGrapher binary (default: work_dir/bin/chrono-grapher) [Modes: Start]"
  echo "  -p|--keeper-bin <path>           Path to the ChronoKeeper binary (default: work_dir/bin/chrono-keeper) [Modes: Start]"
  echo "  -a|--player-bin <path>           Path to the ChronoPlayer binary (default: work_dir/bin/chrono-player) [Modes: Start]"
  echo ""
  echo "Configuration Settings:"
  echo "  -f|--conf-file <path>            Path to the configuration file (default: work_dir/conf/default-chrono-conf.json) [Modes: Start]"
  echo "  -n|--client-conf-file <path>     Path to the client configuration file (default: work_dir/conf/chrono-client-conf.json) [Modes: Start]"
  echo ""
  echo "Miscellaneous Options:"
  echo "  -e|--verbose                     Enable verbose output (default: false)"
  echo ""
  echo "Examples (Assume installing a Debug build to ~/chronolog-install):"
  echo "  1) Builds ChronoLog in Debug mode and installs it to ~/chronolog-install:"
  echo "     $0 --build -t Debug -I ~/chronolog-install"
  echo ""
  echo "  2) Installs ChronoLog to the working directory (~/chronolog-install):"
  echo "     $0 --install -I ~/chronolog-install"
  echo ""
  echo "  3) Starts a ChronoLog deployment using the default configurations and paths,"
  echo "     pulling the binaries, hosts, and configuration files from the specified work directory (~/chronolog-install/chronolog):"
  echo "     $0 --start --work-dir ~/chronolog-install/chronolog"
  echo ""
  echo "  4) Starts a ChronoLog deployment using allocated nodes from Slurm job"
  echo "     job_id from the specified work directory (~/chronolog-install/chronolog),"
  echo "     ChronoVisor will run on the first node, ChronoGrapher on the last node, and ChronoKeeper on all the nodes:"
  echo "     $0 --start --work-dir ~/chronolog-install/chronolog --job-id <job_id>"
  echo ""
  echo "  5) Starts a ChronoLog deployment using allocated nodes from Slurm job job_id,"
  echo "     with 2 ChronoGrapher processes from the specified work directory (~/chronolog-install/chronolog):"
  echo "     $0 --start --work-dir ~/chronolog-install/chronolog --job-id <job_id> --record-groups 2"
  echo ""
  echo "  6) Stops a ChronoLog deployment using the default configurations and paths"
  echo "     from the specified work directory (~/chronolog-install/chronolog):"
  echo "     $0 --stop --work-dir ~/chronolog-install/chronolog"
  echo ""
  exit 1
}

check_op_validity() {
  count=0
  [[ $build == true ]] && count=$((count + 1))
  [[ $install == true ]] && count=$((count + 1))
  [[ $start == true ]] && count=$((count + 1))
  [[ $stop == true ]] && count=$((count + 1))
  [[ $clean == true ]] && count=$((count + 1))

  if [[ $count -ne 1 ]]; then
    echo -e "${ERR}Error: Please select exactly one operation in build (-b), install (-i), start (-d), stop (-s), and clean (-c).${NC}" >&2
    usage
  fi
}

# Main functions
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

# Delegate start/stop/clean operations to deploy_cluster.sh
delegate_to_deploy_cluster() {
  local mode="$1"

  if [[ ! -x "${DEPLOY_CLUSTER_SCRIPT}" ]]; then
    echo -e "${ERR}Error: ${DEPLOY_CLUSTER_SCRIPT} is not executable or not found.${NC}"
    exit 1
  fi

  # Build the argument list
  local args=("${mode}")

  # Pass through relevant parameters
  args+=("--work-dir" "${WORK_DIR}")
  args+=("--record-groups" "${NUM_RECORDING_GROUP}")

  # Pass JOB_ID if set
  [[ -n "${JOB_ID}" ]] && args+=("--job-id" "${JOB_ID}")

  # Pass verbose flag if set
  [[ "${verbose}" == "true" ]] && args+=("--verbose")

  # Pass custom paths if they differ from defaults derived from WORK_DIR
  [[ "${MONITOR_DIR}" != "${WORK_DIR}/monitor" ]] && args+=("--monitor-dir" "${MONITOR_DIR}")
  [[ "${OUTPUT_DIR}" != "${WORK_DIR}/output" ]] && args+=("--output-dir" "${OUTPUT_DIR}")
  [[ "${VISOR_BIN}" != "${WORK_DIR}/bin/chrono-visor" ]] && args+=("--visor-bin" "${VISOR_BIN}")
  [[ "${GRAPHER_BIN}" != "${WORK_DIR}/bin/chrono-grapher" ]] && args+=("--grapher-bin" "${GRAPHER_BIN}")
  [[ "${KEEPER_BIN}" != "${WORK_DIR}/bin/chrono-keeper" ]] && args+=("--keeper-bin" "${KEEPER_BIN}")
  [[ "${PLAYER_BIN}" != "${WORK_DIR}/bin/chrono-player" ]] && args+=("--player-bin" "${PLAYER_BIN}")
  [[ "${CONF_FILE}" != "${WORK_DIR}/conf/default-chrono-conf.json" ]] && args+=("--conf-file" "${CONF_FILE}")
  [[ "${CLIENT_CONF_FILE}" != "${WORK_DIR}/conf/default-chrono-client-conf.json" ]] && args+=("--client-conf-file" "${CLIENT_CONF_FILE}")
  [[ "${VISOR_HOSTS}" != "${WORK_DIR}/conf/hosts_visor" ]] && args+=("--visor-hosts" "${VISOR_HOSTS}")
  [[ "${GRAPHER_HOSTS}" != "${WORK_DIR}/conf/hosts_grapher" ]] && args+=("--grapher-hosts" "${GRAPHER_HOSTS}")
  [[ "${KEEPER_HOSTS}" != "${WORK_DIR}/conf/hosts_keeper" ]] && args+=("--keeper-hosts" "${KEEPER_HOSTS}")

  echo -e "${DEBUG}Delegating to: ${DEPLOY_CLUSTER_SCRIPT} ${args[*]}${NC}"
  "${DEPLOY_CLUSTER_SCRIPT}" "${args[@]}"
}

start() {
  delegate_to_deploy_cluster "--start"
}

stop() {
  delegate_to_deploy_cluster "--stop"
}

clean() {
  delegate_to_deploy_cluster "--clean"
}

parse_args() {
  TEMP=$(getopt -o t:B:I:w:m:u:v:g:p:a:q:k:o:f:n:j:r:hbidsce --long build-type:,build-dir:,install-dir:,work-dir:,monitor-dir:,output-dir:,visor-bin:,grapher-bin:,keeper-bin:,player-bin:,visor-hosts:,grapher-hosts:,keeper-hosts:,conf-file:,client-conf-file:,job-id:,record-groups:,help,build,install,start,stop,clean,verbose -- "$@")
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
      shift 2
      ;;
    -B | --build-dir)
      BUILD_DIR=$(realpath -m "$2")
      shift 2
      ;;
    -I | --install-dir)
      INSTALL_DIR=$(realpath -m "$2")
      shift 2
      ;;
    -w | --work-dir)
      mkdir -p $2
      WORK_DIR=$(realpath -m "${2%/}")
      LIB_DIR="${WORK_DIR}/lib"
      CONF_DIR="${WORK_DIR}/conf"
      BIN_DIR="${WORK_DIR}/bin"
      MONITOR_DIR="${WORK_DIR}/monitor"
      OUTPUT_DIR="${WORK_DIR}/output"
      VISOR_BIN_FILE_NAME="chrono-visor"
      KEEPER_BIN_FILE_NAME="chrono-keeper"
      GRAPHER_BIN_FILE_NAME="chrono-grapher"
      VISOR_BIN="${WORK_DIR}/bin/${VISOR_BIN_FILE_NAME}"
      GRAPHER_BIN="${WORK_DIR}/bin/${GRAPHER_BIN_FILE_NAME}"
      KEEPER_BIN="${WORK_DIR}/bin/${KEEPER_BIN_FILE_NAME}"
      PLAYER_BIN="${WORK_DIR}/bin/${PLAYER_BIN_FILE_NAME}"
      VISOR_BIN_DIR=$(dirname ${VISOR_BIN})
      GRAPHER_BIN_DIR=$(dirname ${GRAPHER_BIN})
      KEEPER_BIN_DIR=$(dirname ${KEEPER_BIN})
      PLAYER_BIN_DIR=$(dirname ${PLAYER_BIN})
      CONF_FILE="${CONF_DIR}/default-chrono-conf.json"
      CLIENT_CONF_FILE="${CONF_DIR}/default-chrono-client-conf.json"
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
    -b | --build)
      build=true
      shift
      ;;
    -i | --install)
      install=true
      shift
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

  # Shift the arguments so that the non-option arguments are left
  shift $((OPTIND - 1))
}

# Parse arguments
parse_args "$@"

# Check if specified operation is allowed
check_op_validity

rc=0

if ${build}; then
  build || rc=$?
elif ${install}; then
  install || rc=$?
elif ${start}; then
  start || rc=$?
elif ${stop}; then
  stop || rc=$?
elif ${clean}; then
  clean || rc=$?
fi

if [ $rc -ne 0 ]; then
  echo -e "${ERR}Operation failed (exit code ${rc})${NC}" >&2
  exit $rc
fi

echo -e "${INFO}Done${NC}"
