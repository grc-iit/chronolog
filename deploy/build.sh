#!/bin/bash

# Exit on any error
set -e

# Default values and color codes
BUILD_TYPE=""
INSTALL_PATH=""  # Installation path is optional and starts empty
REPO_ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/..")"

ERR='\033[7;37m\033[41m'
INFO='\033[7;49m\033[92m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

usage() {
    echo "Usage: $0 -type <BuildType> [-install-path <Path>]"
    echo "Example: $0 -type Debug"
    echo "         $0 -type Release -install-path /custom/install/path"
    exit 1
}

parse_arguments() {
    while [[ "$#" -gt 0 ]]; do
        case $1 in
            -type) BUILD_TYPE="$2"; shift ;;
            -install-path) INSTALL_PATH="$2"; shift ;;
            *) echo "Unknown parameter passed: $1"; usage ;;
        esac
        shift
    done

    # Validate BUILD_TYPE
    if [[ -z "$BUILD_TYPE" || ! "$BUILD_TYPE" =~ ^(Debug|Release)$ ]]; then
        usage
    fi

    # Optional: Validate INSTALL_PATH if specified
    if [[ -n "$INSTALL_PATH" && ! -d "$(dirname "$INSTALL_PATH")" ]]; then
        echo -e "${ERR}Invalid install path: ${INSTALL_PATH}. Parent directory does not exist.${NC}"
        exit 1
    fi
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
    echo -e "${INFO}All dependencies are installed and ready.${NC}"
}

prepare_build_directory() {
    if [ -d "$REPO_ROOT/build" ]; then
        echo -e "${DEBUG}Build directory exists. Removing it...${NC}"
        rm -rf "$REPO_ROOT/build"
    fi
    mkdir "$REPO_ROOT/build"
    cd "$REPO_ROOT/build"
}

build_project() {
    echo -e "${INFO}Building ChronoLog in ${BUILD_TYPE} mode.${NC}"
    if [[ -n "$INSTALL_PATH" ]]; then
        echo -e "${DEBUG}Using installation path: ${INSTALL_PATH}${NC}"
        cmake -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DINSTALL_DIR="${INSTALL_PATH}" ..
    else
        echo -e "${DEBUG}No installation path specified, using default CMake settings [~/home/$USER/chronolog].${NC}"
        cmake -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" ..
    fi
    make all
    echo -e "${DEBUG}ChronoLog Built in ${BUILD_TYPE} mode${NC}"
}

# Main function
main() {
    echo -e "${INFO}Building ChronoLog...${NC}"
    parse_arguments "$@"
    cd ${REPO_ROOT}
    activate_spack_environment
    check_spack
    prepare_build_directory
    build_project
    echo -e "${INFO}ChronoLog Built.${NC}"
}
main "$@"
