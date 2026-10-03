cmake_minimum_required(VERSION 3.25)

project(ChronoLog VERSION 4.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(CHRONOLOG_WERROR "Treat warnings in ChronoLog targets as errors" ON)
option(CHRONOLOG_BUILD_BENCH "Build the benchmark and profiling harness under tools/bench" OFF)
option(CHRONOLOG_BUILD_VISOR "Build the ChronoVisor catalog service" ON)
set(CHRONOLOG_LINK_JOBS 4 CACHE STRING "Concurrent link steps under Ninja; each debug link of a gRPC binary peaks near 3 GB")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS chronolog_link=${CHRONOLOG_LINK_JOBS})
set(CMAKE_JOB_POOL_LINK chronolog_link)
set(CHRONOLOG_SANITIZERS "" CACHE STRING
    "Semicolon separated sanitizer list applied to every target, for example address;undefined or thread")

find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(Threads REQUIRED)
include(cmake/ForbiddenTransport.cmake)
find_program(CHRONOLOG_SETARCH setarch)

if(CHRONOLOG_SANITIZERS)
    string(REPLACE ";" "," _chronolog_san "${CHRONOLOG_SANITIZERS}")
    add_compile_options(-fsanitize=${_chronolog_san} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${_chronolog_san})
    if("undefined" IN_LIST CHRONOLOG_SANITIZERS)
        add_compile_options(-fno-sanitize-recover=undefined)
    endif()
    # ThreadSanitizer aborts with "unexpected memory mapping" when mmap_rnd_bits is wide.
    # Every executable target, including gtest discovery, runs under setarch -R.
    if("thread" IN_LIST CHRONOLOG_SANITIZERS AND CHRONOLOG_SETARCH)
        set(CMAKE_CROSSCOMPILING_EMULATOR "${CHRONOLOG_SETARCH};${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
    endif()
endif()

# Warning flags live on an interface target so they reach only ChronoLog's own
# code. Generated protobuf and gRPC sources never link this target.
add_library(chronolog_warnings INTERFACE)
target_compile_options(chronolog_warnings INTERFACE -Wall -Wextra -Wpedantic)
if(CHRONOLOG_WERROR)
    target_compile_options(chronolog_warnings INTERFACE -Werror)
endif()

# chronolog_proto: C++ messages and gRPC stubs generated from proto/chronolog/v1.
file(GLOB CHRONOLOG_PROTO_FILES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/proto/chronolog/v1/*.proto")
if(NOT CHRONOLOG_PROTO_FILES)
    message(FATAL_ERROR "No .proto files under proto/chronolog/v1")
endif()

set(CHRONOLOG_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${CHRONOLOG_PROTO_GEN_DIR}")

add_library(chronolog_proto STATIC ${CHRONOLOG_PROTO_FILES})
protobuf_generate(
    TARGET chronolog_proto
    LANGUAGE cpp
    IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/proto"
    PROTOC_OUT_DIR "${CHRONOLOG_PROTO_GEN_DIR}")
protobuf_generate(
    TARGET chronolog_proto
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
    IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/proto"
    PROTOC_OUT_DIR "${CHRONOLOG_PROTO_GEN_DIR}")
# SYSTEM keeps warnings in generated headers from failing ChronoLog targets.
target_include_directories(chronolog_proto SYSTEM PUBLIC
    "$<BUILD_INTERFACE:${CHRONOLOG_PROTO_GEN_DIR}>")
target_link_libraries(chronolog_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)

# chronolog_internal_proto: chronolog.internal.v1 (Archive, Cluster). Never
# installed, never exported, never generated into SDKs.
file(GLOB CHRONOLOG_INTERNAL_PROTO_FILES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/proto/chronolog/internal/v1/*.proto")
if(CHRONOLOG_INTERNAL_PROTO_FILES)
    add_library(chronolog_internal_proto STATIC ${CHRONOLOG_INTERNAL_PROTO_FILES})
    protobuf_generate(
        TARGET chronolog_internal_proto
        LANGUAGE cpp
        IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/proto"
        PROTOC_OUT_DIR "${CHRONOLOG_PROTO_GEN_DIR}")
    protobuf_generate(
        TARGET chronolog_internal_proto
        LANGUAGE grpc
        GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
        PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
        IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/proto"
        PROTOC_OUT_DIR "${CHRONOLOG_PROTO_GEN_DIR}")
    target_link_libraries(chronolog_internal_proto PUBLIC chronolog_proto)
endif()

# chronolog_contracts: the frozen C++ contract headers under include/chronolog.
add_library(chronolog_contracts INTERFACE)
target_include_directories(chronolog_contracts INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(chronolog_contracts INTERFACE cxx_std_20)
target_link_libraries(chronolog_contracts INTERFACE absl::status absl::statusor absl::strings)

enable_testing()

file(GLOB CHRONOLOG_CONTRACT_TEST_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/*.cpp")
if(CHRONOLOG_CONTRACT_TEST_SOURCES)
    include(GoogleTest)
    add_executable(chronolog_contract_tests ${CHRONOLOG_CONTRACT_TEST_SOURCES})
    target_link_libraries(chronolog_contract_tests PRIVATE
        chronolog_contracts chronolog_proto chronolog_warnings
        GTest::gtest GTest::gtest_main Threads::Threads)
    # ThreadSanitizer aborts with "unexpected memory mapping" on kernels whose
    # mmap_rnd_bits exceeds what it supports. Disabling ASLR avoids that without
    # changing sysctls, which would need root.
    if("thread" IN_LIST CHRONOLOG_SANITIZERS)
        find_program(CHRONOLOG_SETARCH setarch)
        if(CHRONOLOG_SETARCH)
            set_property(TARGET chronolog_contract_tests PROPERTY CROSSCOMPILING_EMULATOR
                "${CHRONOLOG_SETARCH};${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
        endif()
    endif()
    gtest_discover_tests(chronolog_contract_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST)
else()
    message(STATUS "tests/contract has no sources yet, skipping chronolog_contract_tests")
endif()

add_subdirectory(tools/stub_server)
add_test(NAME build.no_legacy_transport
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NoLegacyTransportCheck.cmake)
set_tests_properties(build.no_legacy_transport PROPERTIES TIMEOUT 30 LABELS "gate")
# The new components live in their own subdirectories.
add_subdirectory(src/chrono-common/clock)
add_subdirectory(src/chrono-common/rpc)
add_subdirectory(src/chrono-keeper/journal)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/chrono-keeper/wal/CMakeLists.txt")
    add_subdirectory(src/chrono-keeper/wal)
endif()
add_subdirectory(src/chrono-keeper/server)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/chrono-keeper/archive/CMakeLists.txt")
    add_subdirectory(src/chrono-keeper/archive)
endif()
add_subdirectory(src/chrono-keeper/tests)
add_subdirectory(src/chrono-player)

if(CHRONOLOG_BUILD_VISOR)
    add_subdirectory(src/chrono-visor)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/chrono-common/tier/CMakeLists.txt")
    add_subdirectory(src/chrono-common/tier)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/chrono-grapher/server/CMakeLists.txt")
    add_subdirectory(src/chrono-grapher/server)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/client/cpp/CMakeLists.txt")
    add_subdirectory(client/cpp)
endif()

if(TARGET chronolog_common_tier)
    add_subdirectory(deploy/cluster)
endif()

if(TARGET chronolog_visor_lib AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/acquisition/CMakeLists.txt")
    add_subdirectory(tests/contract/acquisition)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/proto/CMakeLists.txt")
    add_subdirectory(tests/contract/proto)
endif()

if(CHRONOLOG_BUILD_PYTHON)
    add_subdirectory(client/python/binding)
endif()

if(TARGET chronolog_client AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/client/typescript/test/CMakeLists.txt")
    add_subdirectory(client/typescript/test)
endif()

if((NOT DEFINED CHRONOLOG_BUILD_PLUGINS OR CHRONOLOG_BUILD_PLUGINS) AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/chrono-mcp/CMakeLists.txt")
    add_subdirectory(plugins/chrono-mcp)
endif()

if((NOT DEFINED CHRONOLOG_BUILD_PLUGINS OR CHRONOLOG_BUILD_PLUGINS) AND TARGET chronolog_client)
    add_subdirectory(plugins/chrono-kvs)
endif()

if((NOT DEFINED CHRONOLOG_BUILD_PLUGINS OR CHRONOLOG_BUILD_PLUGINS) AND TARGET chronolog_kvs)
    add_subdirectory(plugins/chrono-pubsub)
endif()

if((NOT DEFINED CHRONOLOG_BUILD_PLUGINS OR CHRONOLOG_BUILD_PLUGINS) AND TARGET chronolog_client)
    add_subdirectory(plugins/chrono-sql)
endif()

if((NOT DEFINED CHRONOLOG_BUILD_PLUGINS OR CHRONOLOG_BUILD_PLUGINS) AND TARGET chronolog_kvs)
    add_subdirectory(plugins/chrono-stream)
endif()

if((NOT DEFINED CHRONOLOG_BUILD_PLUGINS OR CHRONOLOG_BUILD_PLUGINS) AND TARGET chronolog_client)
    add_subdirectory(plugins/chrono-viz)
endif()

if((NOT DEFINED CHRONOLOG_BUILD_PLUGINS OR CHRONOLOG_BUILD_PLUGINS) AND TARGET chronolog_client)
    add_subdirectory(plugins/chrono-ldms)
endif()

if(TARGET chrono_visor AND TARGET chrono_keeper AND TARGET chrono_grapher AND TARGET chrono_player)
    add_subdirectory(tests/integration/dynamic)
    if(TARGET chronolog_client)
        add_subdirectory(tests/integration/keeper_wal)
        add_subdirectory(tests/integration/visor)
        add_subdirectory(tests/integration/lease)
    endif()
    add_test(NAME bind_guard_integration
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/bind_guard_test.sh
        $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_grapher>)
    set_tests_properties(bind_guard_integration PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 180 LABELS "integration")
endif()

if(CHRONOLOG_BUILD_BENCH)
    add_subdirectory(tools/bench)
endif()

if(TARGET chrono_visor AND TARGET chrono_keeper AND TARGET chrono_grapher AND TARGET chrono_player AND TARGET chronolog_client)
    add_subdirectory(deploy/local)
endif()
