cmake_minimum_required(VERSION 3.25)

# Set a consistent MACOSX_RPATH default across all CMake versions.
if(NOT DEFINED CMAKE_MACOSX_RPATH)
    set(CMAKE_MACOSX_RPATH 0)
endif()

project(ChronoLog LANGUAGES CXX C)

#------------------------------------------------------------------------------
# Version information
#------------------------------------------------------------------------------
set(CHRONOLOG_VERSION_MAJOR "0")
set(CHRONOLOG_VERSION_MINOR "7")
set(CHRONOLOG_VERSION_PATCH "0")
set(CHRONOLOG_PACKAGE "ChronoLog")
set(CHRONOLOG_PACKAGE_NAME "CHRONOLOG")
set(CHRONOLOG_PACKAGE_VERSION "${CHRONOLOG_VERSION_MAJOR}.${CHRONOLOG_VERSION_MINOR}.${CHRONOLOG_VERSION_PATCH}")
set(CHRONOLOG_PACKAGE_VERSION_MAJOR "${CHRONOLOG_VERSION_MAJOR}.${CHRONOLOG_VERSION_MINOR}")
set(CHRONOLOG_PACKAGE_VERSION_MINOR "${CHRONOLOG_VERSION_PATCH}")
set(CHRONOLOG_PACKAGE_STRING "${CHRONOLOG_PACKAGE_NAME} ${CHRONOLOG_PACKAGE_VERSION}")
set(CHRONOLOG_PACKAGE_TARNAME "${CHRONOLOG_PACKAGE}")

#------------------------------------------------------------------------------
# Setup install and output Directories
#------------------------------------------------------------------------------
# Validate CMAKE_BUILD_TYPE
if( NOT DEFINED CMAKE_BUILD_TYPE OR NOT (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "Release"))
    message(FATAL_ERROR "Please, define CMAKE_BUILD_TYPE that is either Debug or Release")
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
set(CHRONOLOG_INSTALL_ROOT        "${CMAKE_INSTALL_PREFIX}/chronolog")
set(CHRONOLOG_INSTALL_BIN_DIR     "${CHRONOLOG_INSTALL_ROOT}/bin")
set(CHRONOLOG_INSTALL_LIB_DIR     "${CHRONOLOG_INSTALL_ROOT}/lib")
set(CHRONOLOG_INSTALL_INCLUDE_DIR "${CHRONOLOG_INSTALL_ROOT}/include")
set(CHRONOLOG_INSTALL_CONF_DIR    "${CHRONOLOG_INSTALL_ROOT}/conf")

# Helper function for consistent target installation
function(chronolog_install_target target_name)
    install(TARGETS ${target_name}
        RUNTIME DESTINATION ${CHRONOLOG_INSTALL_BIN_DIR}
        LIBRARY DESTINATION ${CHRONOLOG_INSTALL_LIB_DIR}
        ARCHIVE DESTINATION ${CHRONOLOG_INSTALL_LIB_DIR}
        PUBLIC_HEADER DESTINATION ${CHRONOLOG_INSTALL_INCLUDE_DIR}
    )
    # Set both absolute and relative RPATH for portability
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
#------------------------------------------------------------------------------
if(CHRONOLOG_BUILD_TESTING)
    message(STATUS "Tests enable")
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

if(CHRONOLOG_BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
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
