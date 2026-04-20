#!/bin/bash

# Exit on any error
set -e

# Default values and color codes
BUILD_TYPE="Release"  # Default build type
INSTALL_PATH=""  # Installation path is optional and starts empty
BUILD_BASE_DIR="$HOME/chronolog-build"  # Base build directory
PYTHON_EXECUTABLE=""
REPO_ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../../")"

ERR='\033[7;37m\033[41m'
INFO='\033[7;49m\033[92m'
DEBUG='\033[0;33m'
NC='\033[0m' # No Color

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  -h|--help           Display this help and exit"
    echo ""
    echo "Build Options:"
    echo "  -t|--build-type <Debug|Release>  Define type of build (default: Release)"
    echo "  -B|--build-dir <path>            Set the build directory (default: $HOME/chronolog-build/)"
    echo "  -I|--install-dir <path>          Set the installation directory (default: $HOME/chronolog-install/)"
    echo ""
    echo "Examples:"
    echo ""
    echo "  1) Build ChronoLog in Release mode (default):"
    echo "     $0"
    echo ""
    echo "  2) Build ChronoLog in Debug mode:"
    echo "     $0 --build-type Debug"
    echo ""
    echo "  3) Build ChronoLog in Release mode with custom install directory:"
    echo "     $0 --install-dir /custom/install/path"
    echo ""
    echo "  4) Build ChronoLog in Debug mode with custom build directory:"
    echo "     $0 --build-type Debug --build-dir /custom/build/path"
    echo ""
    exit 1
}

parse_arguments() {
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
                INSTALL_PATH=$(realpath -m "$2")
                shift 2 ;;
            *) 
                echo -e "${ERR}Unknown option: $1${NC}"
                usage ;;
        esac
    done

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

configure_spack_python() {
    local spack_view_python="${REPO_ROOT}/.spack-env/view/bin/python3"

    # Prefer the active environment view directly when available.
    if [[ -x "${spack_view_python}" ]]; then
        PYTHON_EXECUTABLE="${spack_view_python}"
    else
        # Fallback for environments without a view: load python from the active Spack env.
        if ! spack load --first python; then
            echo -e "${ERR}Failed to load Python from the active Spack environment.${NC}"
            exit 1
        fi
        PYTHON_EXECUTABLE="$(command -v python3 || true)"
    fi

    if [[ -z "${PYTHON_EXECUTABLE}" || ! -x "${PYTHON_EXECUTABLE}" ]]; then
        echo -e "${ERR}Unable to resolve a usable python3 executable from Spack.${NC}"
        exit 1
    fi

    local resolved_python
    resolved_python="$("${PYTHON_EXECUTABLE}" -c 'import os,sys; print(os.path.realpath(sys.executable))')"
    if [[ "${resolved_python}" == *"miniconda"* || "${resolved_python}" == *"anaconda"* || "${resolved_python}" == *"/conda/" ]]; then
        echo -e "${ERR}Resolved Python still points to Conda (${resolved_python}).${NC}"
        echo -e "${ERR}Please verify the Spack environment includes Python and is installed.${NC}"
        exit 1
    fi

    echo -e "${INFO}Using Python from Spack: ${resolved_python}${NC}"
}

prepare_build_directory() {
    BUILD_DIR="$BUILD_BASE_DIR/$BUILD_TYPE"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
}

build_project() {
    echo -e "${INFO}Building ChronoLog in ${BUILD_TYPE} mode.${NC}"

    # Configure (always). Debug builds default to installing test executables
    # to chronolog/tests/; any caller can override via the CHRONOLOG_INSTALL_TESTS
    # env var, which we forward as an explicit -D so it takes effect even when
    # reusing a populated build directory (CMake would otherwise keep the
    # cached value and ignore the env var on reconfigure).
    local cmake_args=(-S "${REPO_ROOT}" -B . -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DPython_EXECUTABLE="${PYTHON_EXECUTABLE}")
    if [[ -n "$INSTALL_PATH" ]]; then
        echo -e "${DEBUG}Using installation prefix: ${INSTALL_PATH}${NC}"
        cmake_args+=(-DCMAKE_INSTALL_PREFIX="${INSTALL_PATH}")
    else
        echo -e "${DEBUG}No installation path specified; using CMake defaults (your CMakeLists sets $HOME/chronolog-install).${NC}"
    fi
    # Treat an exported-but-empty CHRONOLOG_INSTALL_TESTS the same as unset
    # (${VAR:+x} expands only when VAR is set AND non-empty), so a stray empty
    # export doesn't silently suppress the Debug-mode default.
    local install_tests=""
    if [[ -n "${CHRONOLOG_INSTALL_TESTS:+x}" ]]; then
        install_tests="${CHRONOLOG_INSTALL_TESTS}"
        echo -e "${DEBUG}CHRONOLOG_INSTALL_TESTS=${install_tests} (from environment).${NC}"
    elif [[ "$BUILD_TYPE" == "Debug" ]]; then
        install_tests="ON"
        echo -e "${DEBUG}Debug build: enabling installation of test executables (CHRONOLOG_INSTALL_TESTS=ON).${NC}"
    fi
    if [[ -n "$install_tests" ]]; then
        cmake_args+=(-DCHRONOLOG_INSTALL_TESTS="${install_tests}")
    fi
    cmake "${cmake_args[@]}"

    # Build (portable + parallel)
    cmake --build . --parallel $(nproc)

    echo -e "${DEBUG}ChronoLog built in ${BUILD_TYPE} mode${NC}"
}

# Main function
main() {
    parse_arguments "$@"
    echo -e "${INFO}Building ChronoLog...${NC}"
    cd ${REPO_ROOT}
    activate_spack_environment
    check_spack
    configure_spack_python
    prepare_build_directory
    build_project
    echo -e "${INFO}ChronoLog Built.${NC}"
}
main "$@"