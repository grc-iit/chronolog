cmake_minimum_required(VERSION 3.25)

find_package(Threads REQUIRED)

#------------------------------------------------------------------------------
# ChronoPubSub Integration Test Executable
#------------------------------------------------------------------------------
add_executable(chronopubsub_integration_test
    chronopubsub_integration_test.cpp
)

target_include_directories(chronopubsub_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Plugins/chronopubsub/include
)

target_link_libraries(chronopubsub_integration_test
    PRIVATE
        chronopubsub
        Threads::Threads
)

target_compile_features(chronopubsub_integration_test PRIVATE
    cxx_std_17
)

set_target_properties(chronopubsub_integration_test PROPERTIES
    OUTPUT_NAME chronolog-test-chronopubsub-integration
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# CTest Configuration
#------------------------------------------------------------------------------
add_test(NAME Integration_ChronoPubSub_PluginIntegration
    COMMAND chronopubsub_integration_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties(Integration_ChronoPubSub_PluginIntegration PROPERTIES
    MANUAL TRUE
    TIMEOUT 600
)

if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronopubsub_integration_test DESTINATION tests)
endif()
