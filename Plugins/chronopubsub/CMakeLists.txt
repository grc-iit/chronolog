cmake_minimum_required(VERSION 3.25)

project(chronopubsub
    VERSION 0.1.0
    DESCRIPTION "ChronoPubSub Plugin for ChronoLog"
    LANGUAGES CXX
)

# Core
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

# Library
add_library(chronopubsub
    src/chronopubsub.cpp
    src/chronopubsub_mapper.cpp
    src/chronopubsub_client_adapter.cpp
)

target_include_directories(chronopubsub
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CHRONOLOG_INSTALL_INCLUDE_DIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

file(GLOB HEADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
set_target_properties(chronopubsub PROPERTIES
    PUBLIC_HEADER "${HEADER_FILES}"
)

set_target_properties(chronopubsub PROPERTIES
    VERSION ${CHRONOLOG_PACKAGE_VERSION}
    SOVERSION ${CHRONOLOG_VERSION_MAJOR}
)

target_include_directories(chronopubsub PRIVATE
    $<TARGET_PROPERTY:chronolog_client,INTERFACE_INCLUDE_DIRECTORIES>
)

find_package(Threads REQUIRED)

target_link_libraries(chronopubsub
    PRIVATE
        chronolog_client
        rt
    PUBLIC
        Threads::Threads
)

# Examples
option(CHRONOPUBSUB_BUILD_EXAMPLES "Build ChronoPubSub examples" ON)
if(CHRONOPUBSUB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

chronolog_install_target(chronopubsub)

message(STATUS "ChronoPubSub: superbuild mode enabled")
