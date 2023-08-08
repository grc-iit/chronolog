#!/bin/bash

CHRONOLOG_ROOT_DIR="/home/${USER}/chronolog"
BIN_DIR="${CHRONOLOG_ROOT_DIR}/bin"
LIB_DIR="${CHRONOLOG_ROOT_DIR}/lib"
CONF_DIR="${CHRONOLOG_ROOT_DIR}/conf"
VISOR_BIN_FILE_NAME="chronovisor_server"
KEEPER_BIN_FILE_NAME="chrono_keeper"
CLIENT_BIN_FILE_NAME="storyteller_test"
VISOR_BIN="${BIN_DIR}/${VISOR_BIN_FILE_NAME}"
KEEPER_BIN="${BIN_DIR}/${KEEPER_BIN_FILE_NAME}"
CLIENT_BIN="${BIN_DIR}/${CLIENT_BIN_FILE_NAME}"
VISOR_HOSTS="${CONF_DIR}/hosts_visor"
KEEPER_HOSTS="${CONF_DIR}/hosts_keeper"
CLIENT_HOSTS="${CONF_DIR}/hosts_client"
CONF_FILE="${CONF_DIR}/default_conf.json"
JOB_ID=""
install=false
deploy=false
reset=false

check_hosts_files() {
    echo "Checking hosts files..."
    if [[ ! -f ${VISOR_HOSTS} ]]
    then
        echo "${VISOR_HOSTS} host file does not exist, exiting ..."
        exit 1
    fi

    if [[ ! -f ${KEEPER_HOSTS} ]]
    then
        echo "${KEEPER_HOSTS} host file does not exist, exiting ..."
        exit 1
    fi

    if [[ ! -f ${CLIENT_HOSTS} ]]
    then
        echo "${CLIENT_HOSTS} host file does not exist, exiting ..."
        exit 1
    fi
}

check_bin_files() {
    echo "Checking binary files..."
    if [[ ! -f ${VISOR_BIN} ]]
    then
        echo "${VISOR_BIN} executable file does not exist, exiting ..."
        exit 1
    fi

    if [[ ! -f ${KEEPER_BIN} ]]
    then
        echo "${KEEPER_BIN} executable file does not exist, exiting ..."
        exit 1
    fi

    if [[ ! -f ${CLIENT_BIN} ]]
    then
        echo "${CLIENT_BIN} executable file does not exist, exiting ..."
        exit 1
    fi
}

check_conf_files() {
    echo "Checking configuration files ..."
    if [[ ! -f ${CONF_FILE} ]]
    then
        echo "${CONF_FILE} configuration file does not exist, exiting ..."
        exit 1
    fi
}

extract_shared_libraries() {
    local executable="$1"
    ldd_output=$(ldd ${executable} 2>/dev/null | awk '{print $3}' | grep -v 'not' | grep -v '^/lib')
    echo "${ldd_output}"
}

copy_shared_libs() {
    libs_visor=$(extract_shared_libraries ${VISOR_BIN})
    libs_keeper=$(extract_shared_libraries ${KEEPER_BIN})
    libs_client=$(extract_shared_libraries ${CLIENT_BIN})

    # Combine shared libraries from all executables and remove duplicates
    all_shared_libs=$(echo -e "${libs_visor}\n${libs_keeper}\n${libs_client}" | sort | uniq)

    echo "Copying shared libraries ..."
    for lib in ${all_shared_libs}
    do
        echo "Copying ${lib} ..."
        cp ${lib} ${BIN_DIR}/
    done
}

install() {
    echo "Installing ..."

    copy_shared_libs

    prepare_hosts

    check_bin_files

    check_conf_files
}

deploy() {
    install

    echo "Deploying ..."

    # launch Visor
    mpssh -f ${VISOR_HOSTS} "nohup ${VISOR_BIN} ${CONF_FILE} > /dev/null 2>&1 &"

    # launch Keeper
    mpssh -f ${KEEPER_HOSTS} "nohup ${KEEPER_BIN} ${CONF_FILE} > /dev/null 2>&1 &"

    # launch Client
    mpssh -f ${CLIENT_HOSTS} "${CLIENT_BIN} ${CONF_FILE}"

    # check Visor
    mpssh -f ${VISOR_HOSTS} "pgrep -fla ${VISOR_BIN_FILE_NAME}"

    # check Keeper
    mpssh -f ${KEEPER_HOSTS} "pgrep -fla ${KEEPER_BIN_FILE_NAME}"

    # check Client
    mpssh -f ${CLIENT_HOSTS} "pgrep -fla ${CLIENT_BIN_FILE_NAME}"
}

reset() {
    prepare_hosts

    echo "Resetting ..."

    # kill Visor
    mpssh -f ${VISOR_HOSTS} "pkill --signal 9 -f ${VISOR_BIN_FILE_NAME}"

    # kill Keeper
    mpssh -f ${KEEPER_HOSTS} "pkill --signal 9 -f ${KEEPER_BIN_FILE_NAME}"

    # kill Client
    mpssh -f ${CLIENT_HOSTS} "pkill --signal 9 -f ${CLIENT_BIN_FILE_NAME}"

    # check Visor
    mpssh -f ${VISOR_HOSTS} "pgrep -fla ${VISOR_BIN_FILE_NAME}"

    # check Keeper
    mpssh -f ${KEEPER_HOSTS} "pgrep -fla ${KEEPER_BIN_FILE_NAME}"

    # check Client
    mpssh -f ${CLIENT_HOSTS} "pgrep -fla ${CLIENT_BIN_FILE_NAME}"

}

parse_args() {
    TEMP=$(getopt -o v:k:c:s:p:t:j:idr --long visor:,keeper:,client:,visor_hosts:,keeper_hosts:,client_hosts:,job_id:,install,deploy,reset -- "$@")

    if [ $? != 0 ] ; then echo "Terminating ..." >&2 ; exit 1 ; fi

    # Note the quotes around '$TEMP': they are essential!
    eval set -- "$TEMP"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -v|--visor)
                VISOR_BIN=$(realpath "$2")
                VISOR_BIN_FILE_NAME=$(basename ${VISOR_BIN})
                shift 2 ;;
            -k|--keeper)
                KEEPER_BIN=$(realpath "$2")
                KEEPER_BIN_FILE_NAME=$(basename ${KEEPER_BIN})
                shift 2 ;;
            -c|--client)
                CLIENT_BIN=$(realpath "$2")
                CLIENT_BIN_FILE_NAME=$(basename ${CLIENT_BIN})
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
            -j|--job_id)
                JOB_ID="$2"
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
                echo "Unknown option: $1"
                exit 1
                ;;
        esac
    done

    if [[ "$install" == true ]]
    then
        install
    fi

    if [[ "$deploy" == true && "$reset" == true ]]
    then
        echo "Error: You must choose between deploy (-d) or reset (-r)."
        usage
        exit 1
    fi

    # Shift the arguments so that the non-option arguments are left
    shift $((OPTIND - 1))

    # Check if required options are provided
    if [[ -z ${VISOR_BIN} || -z ${KEEPER_BIN} || -z ${CLIENT_BIN} ||
          -z ${VISOR_HOSTS} || -z ${KEEPER_HOSTS} || -z ${CLIENT_HOSTS} ]]; then
        echo "Missing required options."
        exit 1
    fi
}

prepare_hosts() {
    if [ -n "$SLURM_JOB_ID" ]
    then
        echo "Launched as a SLURM job, getting hosts from job ${SLURM_JOB_ID} ..."
        hosts="$(echo \"$SLURM_JOB_NODELIST\" | tr ',' '\n')"
        echo "${hosts}" | head -1 > ${VISOR_HOSTS}
        echo "${hosts}" > ${KEEPER_HOSTS}
        echo "${hosts}" > ${CLIENT_HOSTS}
    else
        echo "Launched from a shell, getting hosts from command line or presets ..."
        if [ -n "${JOB_ID}" ]
        then
            echo "JOB_ID is set to be ${JOB_ID} via command line, use it"
            JOB_ID="$(squeue | grep ${JOB_ID} | awk '{print $NF}')"
            hosts="$(scontrol show hostnames ${JOB_ID})"
            echo "${hosts}" | head -1 > ${VISOR_HOSTS}
            echo "${hosts}" > ${KEEPER_HOSTS}
            echo "${hosts}" > ${CLIENT_HOSTS}
        else
            echo "JOB_ID is not set, use presets"
        fi
        check_hosts_files
    fi
}

usage() {
    echo "Usage: $0 -i|--install
                               -d|--deploy
                               -r|--reset
                               -v|--visor VISOR_BIN
                               -k|--keeper KEEPER_BIN
                               -c|--client CLIENT_BIN
                               -s|--visor_hosts VISOR_HOSTS
                               -p|--keeper_hosts KEEPER_HOSTS
                               -r|--client_hosts CLIENT_HOSTS
                               -j|--job_id JOB_ID"
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
    echo "Please select deploy or reset mode"
    usage
fi
