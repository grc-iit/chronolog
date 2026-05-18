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
    ${CMAKE_SOURCE_DIR}/plugins/chrono-kvs/include
)

target_link_libraries(chronokvs_logger_test PRIVATE
    GTest::gtest_main
)

set_target_properties(chronokvs_logger_test PROPERTIES OUTPUT_NAME chronolog-test-chronokvs-logger)
gtest_discover_tests(chronokvs_logger_test TEST_PREFIX "Unit_ChronoKVS_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronokvs_logger_test DESTINATION tests)
endif()
