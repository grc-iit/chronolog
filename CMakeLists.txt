cmake_minimum_required(VERSION 3.25)

project(ChronoLog VERSION 4.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(CHRONOLOG_WERROR "Treat warnings in ChronoLog targets as errors" ON)
set(CHRONOLOG_SANITIZERS "" CACHE STRING
    "Semicolon separated sanitizer list applied to every target, for example address;undefined or thread")

find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)

if(CHRONOLOG_SANITIZERS)
    string(REPLACE ";" "," _chronolog_san "${CHRONOLOG_SANITIZERS}")
    add_compile_options(-fsanitize=${_chronolog_san} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${_chronolog_san})
    if("undefined" IN_LIST CHRONOLOG_SANITIZERS)
        add_compile_options(-fno-sanitize-recover=undefined)
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

# chronolog_contracts: the frozen C++ contract headers under include/chronolog.
add_library(chronolog_contracts INTERFACE)
target_include_directories(chronolog_contracts INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(chronolog_contracts INTERFACE cxx_std_20)

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
    gtest_discover_tests(chronolog_contract_tests DISCOVERY_MODE PRE_TEST)
else()
    message(STATUS "tests/contract has no sources yet, skipping chronolog_contract_tests")
endif()

add_subdirectory(tools/stub_server)
