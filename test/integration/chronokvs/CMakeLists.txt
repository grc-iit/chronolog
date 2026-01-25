cmake_minimum_required(VERSION 3.25)

find_package(Threads REQUIRED)

#------------------------------------------------------------------------------
# ChronoKVS Integration Test Executable
#------------------------------------------------------------------------------
add_executable(chronokvs_integration_test
    chronokvs_integration_test.cpp
)

target_include_directories(chronokvs_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Plugins/chronokvs/include
)

target_link_libraries(chronokvs_integration_test
    PRIVATE
        chronokvs
        chronolog_client
        Threads::Threads
)

target_compile_features(chronokvs_integration_test PRIVATE
    cxx_std_17
)

set_target_properties(chronokvs_integration_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# CTest Configuration
#------------------------------------------------------------------------------
add_test(NAME chronokvs_integration_test
    COMMAND chronokvs_integration_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties(chronokvs_integration_test PROPERTIES
    TIMEOUT 300
    LABELS "integration;chronokvs"
)

#------------------------------------------------------------------------------
# Install test executable
#------------------------------------------------------------------------------
chronolog_install_target(chronokvs_integration_test)
