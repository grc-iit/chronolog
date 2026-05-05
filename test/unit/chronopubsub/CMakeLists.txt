cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# ChronoPubSub unit tests (GTest)
#------------------------------------------------------------------------------
add_executable(chronopubsub_logger_test
    chronopubsub_logger_test.cpp
)

target_include_directories(chronopubsub_logger_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Plugins/chronopubsub/include
)

target_link_libraries(chronopubsub_logger_test PRIVATE
    GTest::gtest_main
)

set_target_properties(chronopubsub_logger_test PROPERTIES OUTPUT_NAME chronolog-test-chronopubsub-logger)
gtest_discover_tests(chronopubsub_logger_test TEST_PREFIX "Unit_ChronoPubSub_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronopubsub_logger_test DESTINATION tests)
endif()

#------------------------------------------------------------------------------
# ChronoPubSub configuration constructor unit tests
# Verifies that the config_path constructor correctly rejects missing/invalid
# configuration files before attempting to connect to ChronoLog. No live
# deployment required.
#------------------------------------------------------------------------------
add_executable(chronopubsub_config_test
    chronopubsub_config_test.cpp
)

target_include_directories(chronopubsub_config_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Plugins/chronopubsub/include
)

target_link_libraries(chronopubsub_config_test PRIVATE
    chronopubsub
    GTest::gtest_main
)

set_target_properties(chronopubsub_config_test PROPERTIES
    OUTPUT_NAME chronolog-test-chronopubsub-config
)
gtest_discover_tests(chronopubsub_config_test TEST_PREFIX "Unit_ChronoPubSub_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronopubsub_config_test DESTINATION tests)
endif()
