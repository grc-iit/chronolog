cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# ChronoKVS unit tests (GTest)
#------------------------------------------------------------------------------
add_executable(chronokvs_logger_test
    chronokvs_logger_test.cpp
)

target_include_directories(chronokvs_logger_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Plugins/chronokvs/include
)

target_link_libraries(chronokvs_logger_test PRIVATE
    GTest::gtest_main
)

set_target_properties(chronokvs_logger_test PROPERTIES OUTPUT_NAME chronolog-test-chronokvs-logger)
gtest_discover_tests(chronokvs_logger_test TEST_PREFIX "Unit_ChronoKVS_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronokvs_logger_test DESTINATION tests)
endif()

#------------------------------------------------------------------------------
# ChronoKVS configuration constructor unit tests
# Verifies that the config_path constructor introduced for issue #470
# correctly rejects missing/invalid configuration files before attempting
# to connect to ChronoLog. No live deployment required.
#------------------------------------------------------------------------------
add_executable(chronokvs_config_test
    chronokvs_config_test.cpp
)

target_include_directories(chronokvs_config_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Plugins/chronokvs/include
)

target_link_libraries(chronokvs_config_test PRIVATE
    chronokvs
    GTest::gtest_main
)

set_target_properties(chronokvs_config_test PROPERTIES
    OUTPUT_NAME chronolog-test-chronokvs-config
)
gtest_discover_tests(chronokvs_config_test TEST_PREFIX "Unit_ChronoKVS_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronokvs_config_test DESTINATION tests)
endif()
