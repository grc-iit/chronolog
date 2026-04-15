cmake_minimum_required(VERSION 3.25)

project(chronokvs
    VERSION 0.1.0
    DESCRIPTION "ChronoKVS Plugin for ChronoLog"
    LANGUAGES CXX
)

# Core
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

# RPATH configuration - use root policy
# Note: Root CMakeLists.txt sets CMAKE_INSTALL_RPATH to ${CHRONOLOG_INSTALL_LIB_DIR}

# Library
add_library(chronokvs
    src/chronokvs.cpp
    src/chronokvs_mapper.cpp
    src/chronokvs_client_adapter.cpp
)

# Public headers use genex for build/install; private headers stay private
target_include_directories(chronokvs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CHRONOLOG_INSTALL_INCLUDE_DIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Set PUBLIC_HEADER property for automatic installation
file(GLOB HEADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
set_target_properties(chronokvs PROPERTIES
    PUBLIC_HEADER "${HEADER_FILES}"
)

# Versioned shared library (symlinks libchronokvs.so, .so.<SOVERSION> created by install)
# Use root ChronoLog version so installed lib aligns with package release
set_target_properties(chronokvs PROPERTIES
    VERSION ${CHRONOLOG_PACKAGE_VERSION}
    SOVERSION ${CHRONOLOG_VERSION_MAJOR}
)

find_package(Threads REQUIRED)

# Link to chrono_common which provides the chronolog::Client wrapper
target_link_libraries(chronokvs
    PRIVATE
        chrono_common
        rt
    PUBLIC
        Threads::Threads
)

# Examples (disabled — require chimaera runtime initialization)
# option(CHRONOKVS_BUILD_EXAMPLES "Build ChronoKVS examples" ON)
# if(CHRONOKVS_BUILD_EXAMPLES)
#     add_subdirectory(examples)
# endif()

# Install (no export set; we're inside the superbuild)
chronolog_install_target(chronokvs)

message(STATUS "ChronoKVS: superbuild mode enabled")
