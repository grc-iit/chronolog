cmake_minimum_required(VERSION 3.25)

# Project definition
project(ChronoKVS
    VERSION 0.1.0
    DESCRIPTION "ChronoKVS Plugin for ChronoLog"
    LANGUAGES CXX
)

# Global properties and compiler settings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Add compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Define source files
set(CHRONOKVS_SOURCES
    src/chronokvs.cpp
    src/chronokvs_mapper.cpp
    src/chronokvs_client_adapter.cpp
)

# Define headers for IDE support
set(CHRONOKVS_HEADERS
    include/chronokvs.h
    include/chronokvs_types.h
    src/chronokvs_mapper.h
    src/chronokvs_client_adapter.h
)

# Library target
add_library(ChronoKVS STATIC
    ${CHRONOKVS_SOURCES}
    ${CHRONOKVS_HEADERS}
)

# Set target properties
set_target_properties(ChronoKVS PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    PUBLIC_HEADER "${CHRONOKVS_HEADERS}"
)

# Include directories
target_include_directories(ChronoKVS
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Dependencies
find_package(Threads REQUIRED)
target_link_libraries(ChronoKVS
    PUBLIC
        chronolog_client
        Threads::Threads
        rt
)

# Installation rules
include(GNUInstallDirs)
install(TARGETS ChronoKVS
    EXPORT ChronoKVSTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/chronokvs
)

# Export targets
install(EXPORT ChronoKVSTargets
    FILE ChronoKVSTargets.cmake
    NAMESPACE ChronoKVS::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ChronoKVS
)

# Examples
option(BUILD_EXAMPLES "Build example programs" ON)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Testing
option(BUILD_TESTING "Build tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()