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

# RPATH so installed example can find libs in ../lib
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
set(CMAKE_BUILD_RPATH  "$ORIGIN;$ORIGIN/../lib")

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
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Bring in any header usage requirements exposed by chronolog_client, without making it public
# (Some ChronoLog releases set PUBLIC includes on the client target.)
target_include_directories(chronokvs PRIVATE
    $<TARGET_PROPERTY:chronolog_client,INTERFACE_INCLUDE_DIRECTORIES>
)

find_package(Threads REQUIRED)

# Link to the in-tree target from the superbuild
target_link_libraries(chronokvs
    PRIVATE
        chronolog_client
        rt
    PUBLIC
        Threads::Threads
)

# Examples
option(CHRONOKVS_BUILD_EXAMPLES "Build ChronoKVS examples" ON)
#if(CHRONOKVS_BUILD_EXAMPLES)
#    add_subdirectory(examples)
#endif()

# Install (no export set; we’re inside the superbuild)
install(TARGETS chronokvs
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

message(STATUS "ChronoKVS: superbuild mode enabled")