#!/bin/bash

# Define some colors
ERR='\033[1;31m\033[41m'
INFO='\033[1,36m\033[42m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

WORK_DIR="/home/${USER}/chronolog"
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
HOSTNAME_HS_NET_POSTFIX="-40g"
JOB_ID=""
install=false
deploy=false
reset=false

check_hosts_files() {
    echo -e "${INFO}Checking hosts files...${NC}"
    if [[ ! -f ${VISOR_HOSTS} ]]
    then
        echo -e "${ERR}${VISOR_HOSTS} host file does not exist, exiting ...${NC}"
        exit 1
    fi

    if [[ ! -f ${KEEPER_HOSTS} ]]
    then
        echo -e "${ERR}${KEEPER_HOSTS} host file does not exist, exiting ...${NC}"
        exit 1
    fi

    if [[ ! -f ${CLIENT_HOSTS} ]]
    then
        echo -e "${ERR}${CLIENT_HOSTS} host file does not exist, exiting ...${NC}"
        exit 1
    fi
}

check_bin_files() {
    echo -e "${INFO}Checking binary files...${NC}"
    if [[ ! -f ${VISOR_BIN} ]]
    then
        echo -e "${ERR}${VISOR_BIN} executable file does not exist, exiting ...${NC}"
        exit 1
    fi

    if [[ ! -f ${KEEPER_BIN} ]]
    then
        echo -e "${ERR}${KEEPER_BIN} executable file does not exist, exiting ...${NC}"
        exit 1
    fi

    if [[ ! -f ${CLIENT_BIN} ]]
    then
        echo -e "${ERR}${CLIENT_BIN} executable file does not exist, exiting ...${NC}"
        exit 1
    fi
}

check_conf_files() {
    echo -e "${INFO}Checking configuration files ...${NC}"
    if [[ ! -f ${CONF_FILE} ]]
    then
        echo -e "${ERR}configuration file ${CONF_FILE} does not exist, exiting ...${NC}"
        exit 1
    fi

    visor_client_portal_rpc_in_visor=$(jq '.chrono_visor.VisorClientPortalService.rpc' "${CONF_FILE}")
    visor_client_portal_rpc_in_client=$(jq '.chrono_client.VisorClientPortalService.rpc' "${CONF_FILE}")
    if [[ "${visor_client_portal_rpc_in_visor}" != "${visor_client_portal_rpc_in_client}" ]]
    then
        echo -e "${ERR}mismatched VisorClientPortalService conf in ${CONF_FILE}, exiting ...${NC}"
        exit 1
    fi

    visor_keeper_registry_rpc_in_visor=$(jq '.chrono_visor.VisorKeeperRegistryService.rpc' "${CONF_FILE}")
    visor_keeper_registry_rpc_in_keeper=$(jq '.chrono_keeper.VisorKeeperRegistryService.rpc' "${CONF_FILE}")
    if [[ "${visor_keeper_registry_rpc_in_visor}" != "${visor_keeper_registry_rpc_in_keeper}" ]]
    then
        echo -e "${ERR}mismatched VisorKeeperRegistryService conf in ${CONF_FILE}, exiting ...${NC}"
        exit 1
    fi
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

get_host_ip() {
    local hostname=$1
    local host_ip=""
    if [[ ${hostname} == *${HOSTNAME_HS_NET_POSTFIX} ]]
    then
        host_ip=$(getent hosts ${hostname} | awk '{print $1}' | head -1)
    else
        host_ip=$(getent hosts ${hostname}${HOSTNAME_HS_NET_POSTFIX} | awk '{print $1}' | head -1)
    fi
    echo "${host_ip}"
}

update_visor_ip() {
    visor_host=$(head -1 ${VISOR_HOSTS})
    visor_ip=$(get_host_ip ${visor_host})
    if [[ -z "${visor_ip}" ]]
    then
        echo -e "${ERR}Cannot get ChronoVisor IP, exiting ...${NC}"
        exit 1
    fi
    echo -e "${INFO}Replacing ChronoVisor IP with ${visor_ip} ...${NC}"
    jq ".chrono_visor.VisorClientPortalService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} > tmp.json && mv tmp.json ${CONF_FILE}
    jq ".chrono_client.VisorClientPortalService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} > tmp.json && mv tmp.json ${CONF_FILE}
    jq ".chrono_visor.VisorKeeperRegistryService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} > tmp.json && mv tmp.json ${CONF_FILE}
    jq ".chrono_keeper.VisorKeeperRegistryService.rpc.service_ip = \"${visor_ip}\"" ${CONF_FILE} > tmp.json && mv tmp.json ${CONF_FILE}
}

generate_conf_for_each_keeper() {
    for keeper_host in $(cat ${KEEPER_HOSTS} | awk '{print $1}')
    do
        keeper_ip=$(get_host_ip ${keeper_host})
        echo -e "${INFO}Generating conf file for Keeper ${keeper_host} ...${NC}"
        jq ".chrono_keeper.KeeperDataStoreAdminService.rpc.service_ip = \"${keeper_ip}\"" ${CONF_FILE} > tmp.json && mv tmp.json ${CONF_FILE}
        jq ".chrono_keeper.KeeperRecordingService.rpc.service_ip = \"${keeper_ip}\"" ${CONF_FILE} > tmp.json && mv tmp.json ${CONF_FILE}
        jq ".chrono_keeper.Logging.log.file = \"chronokeeper_logfile.txt.${keeper_host}\"" ${CONF_FILE} > tmp.json && mv tmp.json ${CONF_FILE}.${keeper_host}
    done
}

install() {
    echo -e "${INFO}Installing ...${NC}"

    copy_shared_libs

    prepare_hosts

    check_bin_files

    check_conf_files

    update_visor_ip

    generate_conf_for_each_keeper
}

deploy() {
    install

    echo -e "${INFO}Deploying ...${NC}"

    # launch Visor
    echo -e "${DEBUG}Lauching ChronoVisor ...${NC}"
    VISOR_BIN="${VISOR_BIN_DIR}/${VISOR_BIN_FILE_NAME}"
    VISOR_ARGS="--config ${CONF_FILE}"
    mpssh -f ${VISOR_HOSTS} "cd ${VISOR_BIN_DIR}; LD_LIBRARY_PATH=${LIB_DIR} nohup ${VISOR_BIN} ${VISOR_ARGS} > ${VISOR_BIN_FILE_NAME}.\$(hostname) 2>&1 &" | grep ares- 2>&1

    # launch Keeper
    echo -e "${DEBUG}Lauching ChronoKeeper ...${NC}"
    KEEPER_BIN="${KEEPER_BIN_DIR}/${KEEPER_BIN_FILE_NAME}"
    KEEPER_ARGS="--config ${CONF_FILE}"
    mpssh -f ${KEEPER_HOSTS} "cd ${KEEPER_BIN_DIR}; LD_LIBRARY_PATH=${LIB_DIR} nohup ${KEEPER_BIN} ${KEEPER_ARGS}.\$(hostname) > ${KEEPER_BIN_FILE_NAME}.\$(hostname) 2>&1 &" | grep ares- 2>&1

    # launch Client
    echo -e "${DEBUG}Lauching Client ...${NC}"
    CLIENT_BIN="${CLIENT_BIN_DIR}/${CLIENT_BIN_FILE_NAME}"
    CLIENT_ARGS="--config ${CONF_FILE}"
    mpssh -f ${CLIENT_HOSTS} "cd ${CLIENT_BIN_DIR}; LD_LIBRARY_PATH=${LIB_DIR} nohup ${CLIENT_BIN} ${CLIENT_ARGS}.\$(hostname) > ${CLIENT_BIN_FILE_NAME}.\$(hostname) 2>&1 &" | grep ares- 2>&1

    # check Visor
    echo -e "${DEBUG}Checking ChronoVisor ...${NC}"
    mpssh -f ${VISOR_HOSTS} "pgrep -fla ${VISOR_BIN_FILE_NAME}" | grep ares- 2>&1

    # check Keeper
    echo -e "${DEBUG}Checking ChronoKeeper ...${NC}"
    mpssh -f ${KEEPER_HOSTS} "pgrep -fla ${KEEPER_BIN_FILE_NAME}" | grep ares- 2>&1

    # check Client
    echo -e "${DEBUG}Checking Client ...${NC}"
    mpssh -f ${CLIENT_HOSTS} "pgrep -fla ${CLIENT_BIN_FILE_NAME}" | grep ares- 2>&1
}

reset() {
    if [[ -z ${JOB_ID} ]]
    then
        echo -e "${INFO}No JOB_ID provided, use hosts files in ${CONF_DIR}${NC}"
        check_hosts_files
    else
        echo -e "${INFO}JOB_ID is provided, prepare hosts file first${NC}"
        prepare_hosts
    fi

    echo -e "${INFO}Resetting ...${NC}"

    # kill Visor
    echo -e "${DEBUG}Killing ChronoVisor ...${NC}"
    mpssh -f ${VISOR_HOSTS} "pkill --signal 9 -f ${VISOR_BIN_FILE_NAME}" | grep ares- 2>&1

    # kill Keeper
    echo -e "${DEBUG}Killing ChronoKeeper ...${NC}"
    mpssh -f ${KEEPER_HOSTS} "pkill --signal 9 -f ${KEEPER_BIN_FILE_NAME}" | grep ares- 2>&1

    # kill Client
    echo -e "${DEBUG}Killing Client ...${NC}"
    mpssh -f ${CLIENT_HOSTS} "pkill --signal 9 -f ${CLIENT_BIN_FILE_NAME}" | grep ares- 2>&1

    # check Visor
    echo -e "${DEBUG}Checking ChronoVisor ...${NC}"
    mpssh -f ${VISOR_HOSTS} "pgrep -fla ${VISOR_BIN_FILE_NAME}" | grep ares- 2>&1

    # check Keeper
    echo -e "${DEBUG}Checking ChronoKeeper ...${NC}"
    mpssh -f ${KEEPER_HOSTS} "pgrep -fla ${KEEPER_BIN_FILE_NAME}" | grep ares- 2>&1

    # check Client
    echo -e "${DEBUG}Checking Client ...${NC}"
    mpssh -f ${CLIENT_HOSTS} "pgrep -fla ${CLIENT_BIN_FILE_NAME}" | grep ares- 2>&1

}

parse_args() {
    TEMP=$(getopt -o w:v:k:c:s:p:t:f:j:hidr --long work_dir:visor:,keeper:,client:,visor_hosts:,keeper_hosts:,client_hosts:,conf_file:,job_id:,help,install,deploy,reset -- "$@")

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
            -v|--visor)
                VISOR_BIN=$(realpath "$2")
                VISOR_BIN_FILE_NAME=$(basename ${VISOR_BIN})
                VISOR_BIN_DIR=$(dirname ${VISOR_BIN})
                shift 2 ;;
            -k|--keeper)
                KEEPER_BIN=$(realpath "$2")
                KEEPER_BIN_FILE_NAME=$(basename ${KEEPER_BIN})
                KEEPER_BIN_DIR=$(dirname ${KEEPER_BIN})
                shift 2 ;;
            -c|--client)
                CLIENT_BIN=$(realpath "$2")
                CLIENT_BIN_FILE_NAME=$(basename ${CLIENT_BIN})
                CLIENT_BIN_DIR=$(dirname ${CLIENT_BIN})
                shift 2 ;;
            -s|--visor_hosts)
                VISOR_HOSTS=$(realpath "$2")
                shift 2 ;;
            -p|--keeper_hosts)
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
            -h|--help)
                usage
                shift 2 ;;
            -i|--install)
                install=true
                shift ;;
            -d|--deploy)
                deploy=true
                shift ;;
            -r|--reset)
                reset=true
                shift ;;
            --)
                shift; break ;;
            *)
                echo -e "${ERR}Unknown option: $1${NC}"
                exit 1
                ;;
        esac
    done

    if [[ "$deploy" == true && "$reset" == true ]]
    then
        echo -e "${ERR}Error: You must choose between deploy (-d) or reset (-r).${NC}"
        usage
        exit 1
    fi

    # Shift the arguments so that the non-option arguments are left
    shift $((OPTIND - 1))

    # Check if required options are provided
    if [[ -z ${VISOR_BIN} || -z ${KEEPER_BIN} || -z ${CLIENT_BIN} ||
          -z ${VISOR_HOSTS} || -z ${KEEPER_HOSTS} || -z ${CLIENT_HOSTS} ]]; then
        echo -e "${ERR}Missing required options.${NC}"
        exit 1
    fi
}

prepare_hosts() {
    if [ -n "$SLURM_JOB_ID" ]
    then
        echo -e "${DEBUG}Launched as a SLURM job, getting hosts from job ${SLURM_JOB_ID} ...${NC}"
        hosts="$(echo \"$SLURM_JOB_NODELIST\" | tr ',' '\n')"
        echo "${hosts}" | head -1 > ${VISOR_HOSTS}
        echo "${hosts}" > ${KEEPER_HOSTS}
        echo "${hosts}" > ${CLIENT_HOSTS}
    else
        echo -e "${DEBUG}Launched from a shell, getting hosts from command line or presets ...${NC}"
        if [ -n "${JOB_ID}" ]
        then
            echo -e "${INFO}JOB_ID is set to be ${JOB_ID} via command line, use it${NC}"
            hosts_regex="$(squeue | grep ${JOB_ID} | awk '{print $NF}')"
            if [[ -z ${hosts_regex} ]]
            then
                echo -e "${ERR}Cannot find job ${JOB_ID}, exiting ...${NC}"
                exit 1
            fi
            hosts="$(scontrol show hostnames ${hosts_regex})"
            echo "${hosts}" | head -1 > ${VISOR_HOSTS}
            echo "${hosts}" > ${KEEPER_HOSTS}
            echo "${hosts}" > ${CLIENT_HOSTS}
        else
            echo -e "${DEBUG}JOB_ID is not set, use presets${NC}"
        fi
        check_hosts_files
    fi
}

usage() {
    echo "Usage: $0 -i|--install Prepare ChronoLog deployment
                               -d|--deploy Start ChronoLog deployment
                               -r|--reset Reset ChronoLog deployment
                               -w|--work_dir WORK_DIR
                               -v|--visor VISOR_BIN
                               -k|--keeper KEEPER_BIN
                               -c|--client CLIENT_BIN
                               -s|--visor_hosts VISOR_HOSTS
                               -p|--keeper_hosts KEEPER_HOSTS
                               -r|--client_hosts CLIENT_HOSTS
                               -f|--conf_file CONF_FILE
                               -j|--job_id JOB_ID
                               -h|--help Print this page"
    exit 1
}

parse_args "$@"

if ${install}
then
    install
fi

if ${deploy}
then
    deploy
elif ${reset}
then
    reset
else
    echo -e "${ERR}Please select deploy or reset mode${NC}"
    usage
fi
echo -e "${INFO}Done${NC}"
