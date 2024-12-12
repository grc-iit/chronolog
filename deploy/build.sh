#!/bin/bash

# Exit on any error
set -e

# Default values and color codes
BUILD_TYPE=""
BUILD_DIR="build"
INSTALL_PATH=""  # Installation path is optional and starts empty

ERR='\033[7;37m\033[41m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

# Function to print usage information
usage() {
    echo "Usage: $0 -type <BuildType> [-install-path <Path>]"
    echo "Example: $0 -type Debug"
    echo "         $0 -type Release -install-path /custom/install/path"
    exit 1
}

# Function to parse arguments
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

# Function to check for Spack and activate the environment
check_spack() {
    if ! command -v spack &> /dev/null; then
        echo -e "${ERR}Spack is not installed or not in the PATH.${NC}"
        echo "Please install Spack and make it accessible in your PATH. Refer to the prerequisites section of the Wiki at https://github.com/grc-iit/ChronoLog/wiki"
        exit 1
    fi

    echo -e "${DEBUG}Activating Spack environment...${NC}"
    spack env activate -p .

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

    # Check if all dependencies are installed
    echo -e "${DEBUG}Ensuring all dependencies are installed...${NC}"
    spack install -v

    # Confirm that everything is ready
    echo -e "${DEBUG}All dependencies are installed and ready.${NC}"
}

# Function to clean and prepare the build directory
prepare_build_directory() {
    if [ -d "$BUILD_DIR" ]; then
        echo -e "${DEBUG}Build directory exists. Removing it...${NC}"
        rm -rf "$BUILD_DIR"
    fi
    mkdir "$BUILD_DIR"
    cd "$BUILD_DIR"
}

# Function to build the project
build_project() {
    echo -e "${DEBUG}Building ChronoLog in ${BUILD_TYPE} mode...${NC}"

    # Determine the appropriate cmake command based on INSTALL_PATH
    if [[ -n "$INSTALL_PATH" ]]; then
        echo -e "${DEBUG}Using installation path: ${INSTALL_PATH}${NC}"
        cmake -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DCMAKE_INSTALL_PREFIX="${INSTALL_PATH}" ..
    else
        echo -e "${DEBUG}No installation path specified, using default CMake settings.${NC}"
        cmake -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" ..
    fi

    make all
    echo -e "${DEBUG}ChronoLog Built in ${BUILD_TYPE} mode${NC}"
}

# Main function
main() {
    parse_arguments "$@"
    cd "$(dirname "$0")/.."
    check_spack
    prepare_build_directory
    build_project
}

# Run the main function with all script arguments
main "$@"
