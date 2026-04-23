cmake_minimum_required(VERSION 3.25)

# Set a consistent MACOSX_RPATH default across all CMake versions.
if(NOT DEFINED CMAKE_MACOSX_RPATH)
    set(CMAKE_MACOSX_RPATH 0)
endif()

project(ChronoLog VERSION 2.6.0 LANGUAGES CXX C)

#------------------------------------------------------------------------------
# Version information
#------------------------------------------------------------------------------
# Derive version variables from CMake project VERSION (set above)
set(CHRONOLOG_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CHRONOLOG_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CHRONOLOG_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CHRONOLOG_PACKAGE "ChronoLog")
set(CHRONOLOG_PACKAGE_NAME "CHRONOLOG")
set(CHRONOLOG_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CHRONOLOG_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
set(CHRONOLOG_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_PATCH}")
set(CHRONOLOG_PACKAGE_STRING "${CHRONOLOG_PACKAGE_NAME} ${PROJECT_VERSION}")
set(CHRONOLOG_PACKAGE_TARNAME "${CHRONOLOG_PACKAGE}")

#------------------------------------------------------------------------------
# Setup install and output Directories
#------------------------------------------------------------------------------
# Validate CMAKE_BUILD_TYPE
if( NOT DEFINED CMAKE_BUILD_TYPE OR NOT (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"))
    message(FATAL_ERROR "Please, define CMAKE_BUILD_TYPE that is either Debug, Release, or RelWithDebInfo")
endif()

message(STATUS "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")

# --- Robust default for CMAKE_INSTALL_PREFIX (only if user didn't set one) ---
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
        set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/chronolog-install" CACHE PATH "Install prefix" FORCE)
    else()
        message(FATAL_ERROR "Cannot determine HOME to set install prefix. Pass -DCMAKE_INSTALL_PREFIX=/path")
    endif()
endif()
message(STATUS "CMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}")

# Single, consistent layout
# bin/ = supported/runtime executables only
# examples/ = runnable examples (installed names: chrono-<component>-example-<role>, same as bin/)
#   - Client: chrono-client-example-writer, -reader, -basic, -distributed-telemetry-writer
#   - ChronoKVS: chrono-chronokvs-example-writer, -reader
# tests/ = test executables (non-release or gated)
#
# NOTE: the subdirectory is intentionally "chronolog" (not "chronolog-VERSION").
# CI/CD, deploy scripts, and Docker scripts all hard-code this stable path as
# WORK_DIR.  The distributable tarball uses "chronolog-VERSION/" internally so
# multiple releases can be extracted side-by-side; that asymmetry is deliberate.
# See cmake/CPackConfig.cmake for a full explanation.
set(CHRONOLOG_INSTALL_ROOT        "${CMAKE_INSTALL_PREFIX}/chronolog")
set(CHRONOLOG_INSTALL_BIN_DIR     "${CHRONOLOG_INSTALL_ROOT}/bin")
set(CHRONOLOG_INSTALL_EXAMPLES_DIR "${CHRONOLOG_INSTALL_ROOT}/examples")
set(CHRONOLOG_INSTALL_TESTS_DIR   "${CHRONOLOG_INSTALL_ROOT}/tests")
set(CHRONOLOG_INSTALL_LIB_DIR     "${CHRONOLOG_INSTALL_ROOT}/lib")
set(CHRONOLOG_INSTALL_INCLUDE_DIR "${CHRONOLOG_INSTALL_ROOT}/include")
set(CHRONOLOG_INSTALL_CONF_DIR    "${CHRONOLOG_INSTALL_ROOT}/conf")

# Helper function for consistent target installation.
# Usage: chronolog_install_target(target_name [DESTINATION bin|examples|tests])
# Default DESTINATION is bin. Use examples/ for sample clients, tests/ for test executables.
function(chronolog_install_target target_name)
    set(_runtime_dest "${CHRONOLOG_INSTALL_BIN_DIR}")
    set(_dest_arg "${ARGV1}")
    if(ARGC GREATER 2 AND ARGV1 STREQUAL "DESTINATION")
        set(_dest_arg "${ARGV2}")
    elseif(ARGC GREATER 1)
        set(_dest_arg "${ARGV1}")
    endif()
    if(NOT _dest_arg STREQUAL "")
        if(_dest_arg STREQUAL "examples")
            set(_runtime_dest "${CHRONOLOG_INSTALL_EXAMPLES_DIR}")
        elseif(_dest_arg STREQUAL "tests")
            set(_runtime_dest "${CHRONOLOG_INSTALL_TESTS_DIR}")
        elseif(NOT _dest_arg STREQUAL "bin")
            message(WARNING "chronolog_install_target(${target_name}): unknown DESTINATION '${_dest_arg}', using bin")
        endif()
    endif()
    install(TARGETS ${target_name}
        RUNTIME DESTINATION ${_runtime_dest}
        LIBRARY DESTINATION ${CHRONOLOG_INSTALL_LIB_DIR}
        ARCHIVE DESTINATION ${CHRONOLOG_INSTALL_LIB_DIR}
        PUBLIC_HEADER DESTINATION ${CHRONOLOG_INSTALL_INCLUDE_DIR}
    )
    # Set both absolute and relative RPATH for portability (examples/tests need ../lib)
    set_target_properties(${target_name} PROPERTIES INSTALL_RPATH "${CHRONOLOG_INSTALL_LIB_DIR};$ORIGIN/../lib")
endfunction()

# Disable using link paths as RPATH
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)

# Set both absolute and relative RPATH for maximum flexibility
set(CMAKE_INSTALL_RPATH "${CHRONOLOG_INSTALL_LIB_DIR};$ORIGIN/../lib")

# Force RUNPATH instead of RPATH (RUNPATH has lower precedence)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--enable-new-dtags")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--enable-new-dtags")

#------------------------------------------------------------------------------
# Disallow in-source build. This checks that there is a defined and separate build folder
#------------------------------------------------------------------------------
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(FATAL_ERROR
        "ChronoLog requires an out of source build."
        "Please create a separate binary directory and run CMake there.")
endif()

# Ensure we're building in the correct build subdirectory based on build type
get_filename_component(BUILD_DIR_NAME "${CMAKE_BINARY_DIR}" NAME)
if(NOT BUILD_DIR_NAME STREQUAL CMAKE_BUILD_TYPE)
    get_filename_component(PARENT_DIR "${CMAKE_BINARY_DIR}" DIRECTORY)
    get_filename_component(PARENT_NAME "${PARENT_DIR}" NAME)
    if(NOT PARENT_NAME STREQUAL "chronolog-build")
        message(WARNING 
            "Recommended build directory structure: ~/chronolog-build/${CMAKE_BUILD_TYPE}\n"
            "Current build directory: ${CMAKE_BINARY_DIR}\n"
            "Consider using: $ENV{HOME}/chronolog-build/${CMAKE_BUILD_TYPE}")
    endif()
endif()

#------------------------------------------------------------------------------
# Set CXX Standard if one was not defined
#------------------------------------------------------------------------------
if(NOT "${CMAKE_CXX_STANDARD}")
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

#------------------------------------------------------------------------------
# Options for compilations
#------------------------------------------------------------------------------
option(SET_WERROR "Elevate warnings to errors" OFF)
option(BUILD_SHARED_LIBS "Build with shared libraries." ON)
option(CHRONOLOG_ENABLE_COVERAGE "Enable code coverage." OFF)
option(CHRONOLOG_USE_ADDRESS_SANITIZER "Enable -fsanitize=address in Debug builds" ON)
option(CHRONOLOG_USE_THREAD_SANITIZER "Enable -fsanitize=thread in Debug builds" OFF)
option(CHRONOLOG_BUILD_TESTING "Build the testing tree." ON)
if(DEFINED ENV{CHRONOLOG_INSTALL_TESTS})
    set(_chronolog_install_tests_default "$ENV{CHRONOLOG_INSTALL_TESTS}")
else()
    set(_chronolog_install_tests_default "OFF")
endif()
option(CHRONOLOG_INSTALL_TESTS "Install test executables to chronolog/tests/ (unit, integration, client)." ${_chronolog_install_tests_default})
option(CHRONOLOG_ENABLE_DOXYGEN "Enable Doxygen documentation generation." OFF)
option(CHRONOLOG_WITH_STREAMING "Build to stream to Grafana through InfluxDB" ON)

#------------------------------------------------------------------------------
# Define the compiler flags
#------------------------------------------------------------------------------
# TODO Should we consider the possibility of a user passing their own flags through CMAKE_CXX_FLAGS
#   Encapsulate everything around if(NOT CMAKE_CXX_FLAGS)
#   Check Hermes for how they delete flags automatically
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Compiling ChronoLog in Debug Mode")
    add_compile_options(-O0)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -O0 -g")
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    # Profiling build: CMake provides -O2 -g -DNDEBUG by default.
    # Only add -lm; do NOT add -O3 so that -O2 and debug symbols are preserved
    # and inlining stays moderate enough for perf annotate to resolve symbols.
    message(STATUS "Compiling ChronoLog in RelWithDebInfo Mode (profiling-friendly)")
    add_compile_options(-lm)
else()
    message(STATUS "Compiling ChronoLog in Release Mode")
    add_compile_options(-O3 -lm)
endif()

#------------------------------------------------------------------------------
# Error Checking on incompatible flags.
#------------------------------------------------------------------------------
if(CHRONOLOG_ENABLE_COVERAGE)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(FATAL_ERROR "Use code coverage with debug mode")
    endif()
endif()

#------------------------------------------------------------------------------
# Transform flags from boolean to meaningful usages.
#------------------------------------------------------------------------------
if(BUILD_SHARED_LIBS)
    set(CHRONOLOG_LIBTYPE SHARED)
else()
    set(CHRONOLOG_LIBTYPE STATIC)
endif()

#------------------------------------------------------------------------------
# Coverage
#------------------------------------------------------------------------------
if(CHRONOLOG_ENABLE_COVERAGE)
    set(COVERAGE_FLAGS "-fprofile-arcs -ftest-coverage" CACHE STRING
        "Flags to the coverage program to perform coverage inspection")
    mark_as_advanced(COVERAGE_FLAGS)

    macro(set_coverage_flags target)
        set_target_properties(${target}
            PROPERTIES
            COMPILE_FLAGS ${COVERAGE_FLAGS}
            LINK_FLAGS ${COVERAGE_FLAGS}
        )
    endmacro()
endif()

#------------------------------------------------------------------------------
# Documentation
#------------------------------------------------------------------------------

if(CHRONOLOG_ENABLE_DOXYGEN)
    message(STATUS "Doxygen enable, generating documentation")
    find_package(Doxygen REQUIRED)
    include(UseDoxygenDoc)

    add_doxygen_doc(
        BUILD_DIR
        ${CMAKE_CURRENT_BINARY_DIR}/_build
        DOXY_FILE
        ${CMAKE_CURRENT_SOURCE_DIR}/doc/Doxyfile.in
        TARGET_NAME
        dox
        COMMENT
        "HTML documentation"
    )
endif()

#------------------------------------------------------------------------------
# Linting
#------------------------------------------------------------------------------
add_custom_target(lint COMMAND cpplint --recursive
    --exclude=${CMAKE_CURRENT_SOURCE_DIR}/build
    --exclude=${CMAKE_CURRENT_SOURCE_DIR}/cmake-build-debug
    --exclude=${CMAKE_CURRENT_SOURCE_DIR}/external_libs
    --exclude=${CMAKE_CURRENT_SOURCE_DIR}/doc
    --exclude=${CMAKE_CURRENT_SOURCE_DIR}/CI
    ${CMAKE_CURRENT_SOURCE_DIR})

#------------------------------------------------------------------------------
# Testing
#
# The test tree is built in both Debug and Release so CI (and anyone who wants
# to exercise a release-mode binary) can run the suite against the real build.
# Tests are NOT installed by default: CHRONOLOG_INSTALL_TESTS controls whether
# they land in the install prefix, and it defaults to OFF so release packages
# (TGZ/RPM/DEB) stay clean. Opt in via -DCHRONOLOG_INSTALL_TESTS=ON.
#
# The CHRONOLOG_INSTALL_TESTS env var only seeds the default on a fresh
# configure; CMake caches the option, so the env var is ignored on
# reconfigure. tools/deploy/ChronoLog/build.sh forwards the env var as an
# explicit -D on every invocation so it wins even when the build dir is
# reused — if you invoke cmake directly, pass -D yourself.
#------------------------------------------------------------------------------
set(CHRONOLOG_BUILD_TESTS_NOW ${CHRONOLOG_BUILD_TESTING})
if(CHRONOLOG_BUILD_TESTS_NOW)
    message(STATUS "Tests enabled (install: ${CHRONOLOG_INSTALL_TESTS})")
    include(CTest)
endif()

#------------------------------------------------------------------------------
# ChronoLog Components
#------------------------------------------------------------------------------
add_subdirectory(chrono_common)
add_subdirectory(Client)
add_subdirectory(ChronoVisor)
add_subdirectory(ChronoKeeper)
add_subdirectory(ChronoGrapher)
add_subdirectory(ChronoPlayer)
add_subdirectory(ChronoStore)
add_subdirectory(tools)
add_subdirectory(Plugins)

if(CHRONOLOG_BUILD_TESTS_NOW)
    enable_testing()
    add_subdirectory(test)
else()
    # Performance regression harness ships in every build type (including
    # Release) so the perf matrix can be run against optimized binaries. The
    # rest of the test tree remains Debug-only.
    add_subdirectory(test/performance)
endif()

#------------------------------------------------------------------------------
# CMake Package Configuration Files
#------------------------------------------------------------------------------
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

# Generate the version file
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ChronologConfigVersion.cmake"
    VERSION ${CHRONOLOG_PACKAGE_VERSION}
    COMPATIBILITY AnyNewerVersion
)

# Configure the config file
# First, create a version-aware intermediate file
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ChronologConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ChronologConfig.cmake.intermediate"
    @ONLY
)

# Then use configure_package_config_file to add PACKAGE_INIT and handle PATH_VARS
# This will process @PACKAGE_INIT@ and @PACKAGE_CHRONOLOG_INSTALL_*@ variables
configure_package_config_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ChronologConfig.cmake.intermediate"
    "${CMAKE_CURRENT_BINARY_DIR}/ChronologConfig.cmake"
    INSTALL_DESTINATION ${CHRONOLOG_INSTALL_ROOT}/lib/cmake/Chronolog
    PATH_VARS CHRONOLOG_INSTALL_INCLUDE_DIR CHRONOLOG_INSTALL_LIB_DIR
)

# Install CMake config files
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ChronologConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ChronologConfigVersion.cmake"
    DESTINATION ${CHRONOLOG_INSTALL_ROOT}/lib/cmake/Chronolog
)

#------------------------------------------------------------------------------
# pkg-config Support
#------------------------------------------------------------------------------
# Configure the pkg-config file
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/chronolog.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/chronolog.pc"
    @ONLY
)

# Install pkg-config file
# Install to chronolog/lib/pkgconfig to match the installation structure
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/chronolog.pc"
    DESTINATION ${CHRONOLOG_INSTALL_ROOT}/lib/pkgconfig
)

#------------------------------------------------------------------------------
# Runtime Version Header
#------------------------------------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/chronolog_version.h"
    @ONLY
)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/chronolog_version.h"
    DESTINATION ${CHRONOLOG_INSTALL_INCLUDE_DIR}/chronolog
    RENAME version.h
)

#------------------------------------------------------------------------------
# License and README (for packaging and install tree)
#------------------------------------------------------------------------------
install(
    FILES
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "${CMAKE_SOURCE_DIR}/README.md"
    DESTINATION ${CHRONOLOG_INSTALL_ROOT}
)

#------------------------------------------------------------------------------
# Deploy-from-install helper scripts
#------------------------------------------------------------------------------
install(
    PROGRAMS
        tools/deploy/ChronoLog/deploy_local.sh
        tools/deploy/ChronoLog/deploy_cluster.sh
    DESTINATION ${CHRONOLOG_INSTALL_ROOT}/tools
)

#------------------------------------------------------------------------------
# Custom Targets
#------------------------------------------------------------------------------
add_custom_target(make_all cd ${CMAKE_BINARY_DIR} && make)

#------------------------------------------------------------------------------
# Packaging (must come after all install() calls)
#------------------------------------------------------------------------------
include(cmake/CPackConfig.cmake)
include(CPack)

# --- Per-generator CPack config files ----------------------------------------
# CPACK_INSTALLED_DIRECTORIES cannot be overridden per generator, so RPM and DEB
# each get their own config file configured here and invoked separately by the
# package_release target.  TGZ continues to use the auto-generated CPackConfig.cmake.

# Map CMAKE_SYSTEM_PROCESSOR to Debian architecture name (x86_64 → amd64, etc.)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(CHRONOLOG_DEB_ARCH "amd64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(CHRONOLOG_DEB_ARCH "arm64")
else()
    set(CHRONOLOG_DEB_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/CPackRPMConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/CPackRPMConfig.cmake"
    @ONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/CPackDEBConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/CPackDEBConfig.cmake"
    @ONLY
)

# --- package_release target --------------------------------------------------
# Produces: chronolog-<version>-<system>-<processor>.tar.gz  (always)
#           chronolog-<version>-<system>-<processor>.rpm      (if rpmbuild present)
#           chronolog-<version>-<system>-<processor>.deb      (if dpkg-deb present)
# Usage: cmake --build . --target package_release
get_filename_component(_cmake_bin_dir "${CMAKE_COMMAND}" DIRECTORY)
find_program(_cpack_program cpack HINTS "${_cmake_bin_dir}" REQUIRED)
find_program(CHRONOLOG_RPMBUILD rpmbuild)
find_program(CHRONOLOG_DPKG_DEB dpkg-deb)

string(TOLOWER "${CMAKE_SYSTEM_NAME}" _system_lower)
set(_pkg_base "${CPACK_PACKAGE_NAME}-${CHRONOLOG_PACKAGE_VERSION}")

# TGZ output name
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_tgz_name "${_pkg_base}-${_system_lower}-${CMAKE_SYSTEM_PROCESSOR}-debug")
else()
    set(_tgz_name "${_pkg_base}-${_system_lower}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

# RPM output names (CPack produces name-ver-release.arch.rpm)
set(_rpm_src  "${_pkg_base}-1.${CMAKE_SYSTEM_PROCESSOR}.rpm")
set(_rpm_dest "${_pkg_base}-${_system_lower}-${CMAKE_SYSTEM_PROCESSOR}.rpm")

# DEB output names (CPack produces name_ver-release_arch.deb internally)
# Renamed on output to match the tarball/RPM convention: name-ver-system-processor.deb
# Debug builds get a -debug suffix so release and debug DEBs can coexist as artifacts.
set(_deb_src  "chronolog_${CHRONOLOG_PACKAGE_VERSION}-1_${CHRONOLOG_DEB_ARCH}.deb")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_deb_dest "${_pkg_base}-${_system_lower}-${CMAKE_SYSTEM_PROCESSOR}-debug.deb")
else()
    set(_deb_dest "${_pkg_base}-${_system_lower}-${CMAKE_SYSTEM_PROCESSOR}.deb")
endif()

if(CHRONOLOG_RPMBUILD)
    message(STATUS "CPack: rpmbuild found at ${CHRONOLOG_RPMBUILD} — RPM packages will be built")
else()
    message(STATUS "CPack: rpmbuild not found — RPM packages will be skipped")
endif()
if(CHRONOLOG_DPKG_DEB)
    message(STATUS "CPack: dpkg-deb found at ${CHRONOLOG_DPKG_DEB} — DEB packages will be built")
else()
    message(STATUS "CPack: dpkg-deb not found — DEB packages will be skipped")
endif()

# TGZ (always)
add_custom_target(package_release
    COMMAND "${_cpack_program}" --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
            -G TGZ -B "${CMAKE_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E rename
            "${CMAKE_BINARY_DIR}/${_pkg_base}.tar.gz"
            "${CMAKE_BINARY_DIR}/${_tgz_name}.tar.gz"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Creating ${_tgz_name}.tar.gz"
    VERBATIM
)

# RPM (when rpmbuild is available)
if(CHRONOLOG_RPMBUILD)
    add_custom_command(TARGET package_release POST_BUILD
        COMMAND "${_cpack_program}" --config "${CMAKE_BINARY_DIR}/CPackRPMConfig.cmake"
                -G RPM -B "${CMAKE_BINARY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E rename
                "${CMAKE_BINARY_DIR}/${_rpm_src}"
                "${CMAKE_BINARY_DIR}/${_rpm_dest}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Creating ${_rpm_dest}"
        VERBATIM
    )
endif()

# DEB (when dpkg-deb is available)
if(CHRONOLOG_DPKG_DEB)
    add_custom_command(TARGET package_release POST_BUILD
        COMMAND "${_cpack_program}" --config "${CMAKE_BINARY_DIR}/CPackDEBConfig.cmake"
                -G DEB -B "${CMAKE_BINARY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E rename
                "${CMAKE_BINARY_DIR}/${_deb_src}"
                "${CMAKE_BINARY_DIR}/${_deb_dest}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Creating ${_deb_dest}"
        VERBATIM
    )
endif()

# Source tarball is built via the CPack-provided `package_source` target
# (auto-generated when CPACK_SOURCE_GENERATOR is set in cmake/CPackConfig.cmake).
# Output: chronolog-<version>-source.tar.gz
# Usage: cmake --build . --target package_source
