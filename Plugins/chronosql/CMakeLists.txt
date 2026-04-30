cmake_minimum_required(VERSION 3.25)

project(chronosql
    VERSION 0.1.0
    DESCRIPTION "ChronoSQL Plugin for ChronoLog"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

add_library(chronosql
    src/chronosql.cpp
    src/chronosql_mapper.cpp
    src/chronosql_client_adapter.cpp
    src/chronosql_metadata.cpp
    src/chronosql_row_codec.cpp
    src/chronosql_sql_parser.cpp
)

target_include_directories(chronosql
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CHRONOLOG_INSTALL_INCLUDE_DIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Public headers for installation
file(GLOB HEADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
set_target_properties(chronosql PROPERTIES
    PUBLIC_HEADER "${HEADER_FILES}"
)

set_target_properties(chronosql PROPERTIES
    VERSION ${CHRONOLOG_PACKAGE_VERSION}
    SOVERSION ${CHRONOLOG_VERSION_MAJOR}
)

# Bring in chronolog_client's interface headers (json-c, ClientConfiguration, ...).
target_include_directories(chronosql PRIVATE
    $<TARGET_PROPERTY:chronolog_client,INTERFACE_INCLUDE_DIRECTORIES>
)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON-C REQUIRED json-c)

target_include_directories(chronosql PRIVATE ${JSON-C_INCLUDE_DIRS})
target_link_directories(chronosql PRIVATE ${JSON-C_LIBRARY_DIRS})

target_link_libraries(chronosql
    PRIVATE
        chronolog_client
        ${JSON-C_LIBRARIES}
        rt
    PUBLIC
        Threads::Threads
)

option(CHRONOSQL_BUILD_EXAMPLES "Build ChronoSQL examples" ON)
if(CHRONOSQL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

chronolog_install_target(chronosql)

message(STATUS "ChronoSQL: superbuild mode enabled")
