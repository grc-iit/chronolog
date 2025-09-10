cmake_minimum_required(VERSION 3.25)

project(chronokvs
    VERSION 0.1.0
    DESCRIPTION "ChronoKVS Plugin for ChronoLog"
    LANGUAGES CXX
)

# Core settings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Library target
add_library(chronokvs
    src/chronokvs.cpp
    src/chronokvs_mapper.cpp
    src/chronokvs_client_adapter.cpp
)

target_include_directories(chronokvs
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/Client/include
)

# Dependencies
find_package(Threads REQUIRED)
target_link_libraries(chronokvs
    PRIVATE
        chronolog_client
    PUBLIC
        Threads::Threads
        rt
)

# Examples option
option(CHRONOKVS_BUILD_EXAMPLES "Build ChronoKVS examples" ON)
if(CHRONOKVS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Status message
message(STATUS "ChronoKVS Configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Examples: ${CHRONOKVS_BUILD_EXAMPLES}")