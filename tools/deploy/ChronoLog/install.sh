#!/bin/bash

# Exit on any error
set -e

# Color codes for messages
ERR='\033[7;37m\033[41m'
INFO='\033[7;49m\033[92m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

# Default values
BUILD_TYPE="Release"
BUILD_BASE_DIR="$HOME/chronolog-build"
INSTALL_DIR="$HOME/chronolog-install"

# Directories
REPO_ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../../")"
LIB_DIR=""
BIN_DIR=""

parse_arguments() {
    usage() {
        echo "Usage: $0 [options]"
        echo ""
        echo "Options:"
        echo "  -h|--help           Display this help and exit"
        echo ""
        echo "Install Options:"
        echo "  -t|--build-type <Debug|Release>  Define type of build (default: Release)"
        echo "  -B|--build-dir <path>            Set the build directory (default: $HOME/chronolog-build/)"
        echo "  -I|--install-dir <path>          Set the installation directory (default: $HOME/chronolog-install/)"
        echo ""
        echo "Examples:"
        echo ""
        echo "  1) Install ChronoLog in Release mode (default):"
        echo "     $0"
        echo ""
        echo "  2) Install ChronoLog in Debug mode:"
        echo "     $0 --build-type Debug"
        echo ""
        echo "  3) Install ChronoLog in Release mode with custom install directory:"
        echo "     $0 --install-dir /custom/install/path"
        echo ""
        echo "  4) Install ChronoLog in Debug mode with custom build directory:"
        echo "     $0 --build-type Debug --build-dir /custom/build/path"
        echo ""
        echo "Note: Installation will overwrite any existing installation in the specified directories."
        echo "Debug and Release builds cannot coexist in the installation directory."
        exit 1
    }

    while [[ "$#" -gt 0 ]]; do
        case "$1" in
            -h|--help)
                usage
                shift ;;
            -t|--build-type)
                BUILD_TYPE="$2"
                if [[ "$BUILD_TYPE" != "Debug" && "$BUILD_TYPE" != "Release" ]]; then
                    echo -e "${ERR}Invalid build type: $BUILD_TYPE. Must be 'Debug' or 'Release'.${NC}"
                    usage
                fi
                shift 2 ;;
            -B|--build-dir)
                BUILD_BASE_DIR=$(realpath -m "$2")
                shift 2 ;;
            -I|--install-dir)
                INSTALL_DIR=$(realpath -m "$2")
                shift 2 ;;
            *) 
                echo -e "${ERR}Unknown option: $1${NC}"
                usage ;;
        esac
    done

    # Set default directories
    BIN_DIR="${INSTALL_DIR}/chronolog/bin"
    LIB_DIR="${INSTALL_DIR}/chronolog/lib"
    
    echo -e "${DEBUG}Using build type: ${BUILD_TYPE}${NC}"
    echo -e "${DEBUG}Using build directory: ${BUILD_BASE_DIR}/${BUILD_TYPE}${NC}"
    echo -e "${DEBUG}Using install directory: ${INSTALL_DIR}${NC}"
    echo -e "${DEBUG}Using binary directory: ${BIN_DIR}${NC}"
    echo -e "${DEBUG}Using library directory: ${LIB_DIR}${NC}"
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
        cp -n "$lib_path" "$dest_path/"
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

update_rpath() {
    echo -e "${DEBUG}Updating RPATH...${NC}"

    # Update RPATH for binaries
    shopt -s nullglob
    bin_files=("${BIN_DIR}"/*)
    if [ ${#bin_files[@]} -eq 0 ]; then
        echo -e "${INFO}No binaries found in ${BIN_DIR} to update RPATH.${NC}"
    else
        for f in "${bin_files[@]}"; do
            if [ -f "$f" ]; then
                chrpath -r '$ORIGIN/../lib' "$f" > /dev/null 2>&1 || \
                    echo -e "${INFO}Skipping RPATH update for $f (no RPATH or not a valid ELF file).${NC}"
            fi
        done
    fi

    # Update RPATH for libraries
    lib_files=("${LIB_DIR}"/*)
    if [ ${#lib_files[@]} -eq 0 ]; then
        echo -e "${INFO}No libraries found in ${LIB_DIR} to update RPATH.${NC}"
    else
        for f in "${lib_files[@]}"; do
            if [ -f "$f" ]; then
                chrpath -r '$ORIGIN/../lib' "$f" > /dev/null 2>&1 || \
                    echo -e "${INFO}Skipping RPATH update for $f (no RPATH or not a valid ELF file).${NC}"
            fi
        done
    fi
    shopt -u nullglob
    echo -e "${DEBUG}RPATH updated${NC}"
}

check_dependencies() {
    local dependencies=("jq" "ldd" "nohup" "pkill" "readlink" "realpath" "chrpath")
    echo -e "${DEBUG}Checking required dependencies...${NC}"
    for dep in "${dependencies[@]}"; do
        if ! command -v $dep &> /dev/null; then
            echo -e "${ERR}Dependency $dep is not installed. Please install it and try again.${NC}"
            exit 1
        fi
    done
    echo -e "${DEBUG}All required dependencies are installed.${NC}"
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
    BUILD_DIR="$BUILD_BASE_DIR/${BUILD_TYPE}"
    
    # Check if the build directory exists
    if [ -d "${BUILD_DIR}" ] && [ -f "${BUILD_DIR}/CMakeCache.txt" ]; then
        echo -e "${DEBUG}Using build directory: ${BUILD_DIR}${NC}"
        cd ${BUILD_DIR}
    else
        echo -e "${ERR}Build directory not found: ${BUILD_DIR}${NC}"
        echo -e "${ERR}Please run the build script first with the appropriate build type.${NC}"
        exit 1
    fi
}

# Main function
main() {
    parse_arguments "$@"
    echo -e "${INFO}Installing ChronoLog...${NC}"
    cd ${REPO_ROOT}
    check_dependencies
    activate_spack_environment
    check_spack
    navigate_to_build_directory
    make install
    copy_shared_libs
    update_rpath
    echo -e "${INFO}ChronoLog Installed.${NC}"
}
main "$@"
