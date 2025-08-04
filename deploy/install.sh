#!/bin/bash

# Exit on any error
set -e

# Color codes for messages
ERR='\033[7;37m\033[41m'
INFO='\033[7;49m\033[92m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

# Directories
REPO_ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/..")"
LIB_DIR=""
BIN_DIR=""

parse_arguments() {
    usage() {
        echo -e "Usage: $0 --bin-dir <BIN_DIR> --lib-dir <LIB_DIR>"
        echo -e "\nOptions:"
        echo -e "  --bin-dir <BIN_DIR>   Path to the directory containing binaries."
        echo -e "  --lib-dir <LIB_DIR>   Path to the directory where shared libraries will be copied."
        exit 1
    }

    while [[ "$#" -gt 0 ]]; do
        case "$1" in
            --bin-dir)
                BIN_DIR="$2"
                shift 2
                ;;
            --lib-dir)
                LIB_DIR="$2"
                shift 2
                ;;
            --help)
                usage
                ;;
            *)
                echo -e "${ERR}Unknown argument: $1${NC}"
                usage
                ;;
        esac
    done

    # Ensure required variables are set
    if [[ -z "${BIN_DIR}" || -z "${LIB_DIR}" ]]; then
        echo -e "${ERR}Missing required arguments. Please provide --bin-dir and --lib-dir.${NC}"
        usage
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
    local linked_to_lib_path

    linked_to_lib_path="$(readlink -f ${lib_path})"
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

activate_spack_environment() {
    # Verify Spack is available after sourcing
    if ! command -v spack &> /dev/null; then
        echo -e "${ERR}Spack is not properly loaded into the environment.${NC}"
        exit 1
    fi

    # Activate the Spack environment
    echo -e "${DEBUG}Activating Spack environment in '${REPO_ROOT}'...${NC}"
    if eval $(spack env activate --sh "${REPO_ROOT}"); then
        echo -e "${INFO}Spack environment activated successfully.${NC}"
    else
        echo -e "${ERR}Failed to activate Spack environment. Ensure it is properly configured.${NC}"
        exit 1
    fi
}

check_spack() {
    # Check if an environment is active
    if spack env status | grep -q "No active environment"; then
        echo -e "${DEBUG}No active Spack environment found. Exiting.${NC}"
        exit 1
    fi

    # Concretize the environment to check for dependencies
    echo -e "${DEBUG}Checking dependencies...${NC}"
    if ! spack concretize; then
        echo -e "${DEBUG}Spack concretization failed. Exiting.${NC}"
        exit 1
    fi

    # Ensuring dependencies installation
    echo -e "${DEBUG}Ensuring all dependencies are installed...${NC}"
    spack install -v
    echo -e "${INFO}All dependencies are installed and ready. Installing...${NC}"
}

navigate_to_build_directory() {
    echo -e "${DEBUG}Checking for the presence of the 'build' directory...${NC}"
    if [ -d "${REPO_ROOT}/build" ]; then
        echo -e "${DEBUG}'build' directory found. Navigating to it...${NC}"
        cd ${REPO_ROOT}/build
    else
        echo -e "${ERR}Build directory not found. Please run the build script first.${NC}"
        exit 1
    fi
}

# Main function
main() {
    echo -e "${INFO}Installing ChronoLog...${NC}"
    parse_arguments "$@"
    cd ${REPO_ROOT}
    activate_spack_environment
    check_spack
    navigate_to_build_directory
    make install
    copy_shared_libs
    echo -e "${INFO}ChronoLog Installed.${NC}"
}
main "$@"
