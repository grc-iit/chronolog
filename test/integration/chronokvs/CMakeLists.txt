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
        Threads::Threads
)

target_compile_features(chronokvs_integration_test PRIVATE
    cxx_std_17
)

set_target_properties(chronokvs_integration_test PROPERTIES
    OUTPUT_NAME chronolog-test-chronokvs-integration
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# CTest Configuration
#------------------------------------------------------------------------------
add_test(NAME Integration_ChronoKVS_PluginIntegration
    COMMAND chronokvs_integration_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties(Integration_ChronoKVS_PluginIntegration PROPERTIES
    MANUAL TRUE
    TIMEOUT 300
)

if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronokvs_integration_test DESTINATION tests)
endif()
