cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# chronolog_client unit tests (GTest)
#------------------------------------------------------------------------------

# ClientIdentity pack/unpack
add_executable(chronolog_client_identity_test
    chronolog_client_identity_test.cpp
)

target_include_directories(chronolog_client_identity_test PRIVATE
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(chronolog_client_identity_test PRIVATE
    GTest::gtest_main
    chronolog_client
)

set_target_properties(chronolog_client_identity_test PROPERTIES OUTPUT_NAME chronolog-test-client-identity)
gtest_discover_tests(chronolog_client_identity_test TEST_PREFIX "Unit_ChronologClient_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronolog_client_identity_test DESTINATION tests)
endif()

# Address a reader-mode client's query service listens on
add_executable(chronolog_client_query_service_address_test
    query_service_address_test.cpp
)

target_include_directories(chronolog_client_query_service_address_test PRIVATE
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${CMAKE_SOURCE_DIR}/client/cpp/src
)

target_link_libraries(chronolog_client_query_service_address_test PRIVATE
    GTest::gtest_main
    chronolog_client
)

set_target_properties(chronolog_client_query_service_address_test PROPERTIES OUTPUT_NAME chronolog-test-client-query-service-address)
gtest_discover_tests(chronolog_client_query_service_address_test TEST_PREFIX "Unit_ChronologClient_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronolog_client_query_service_address_test DESTINATION tests)
endif()

# User name a client logs at start, with or without a controlling terminal
add_executable(chronolog_client_login_name_test
    client_login_name_test.cpp
)

target_include_directories(chronolog_client_login_name_test PRIVATE
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${CMAKE_SOURCE_DIR}/client/cpp/src
)

target_link_libraries(chronolog_client_login_name_test PRIVATE
    GTest::gtest_main
    chronolog_client
)

set_target_properties(chronolog_client_login_name_test PROPERTIES OUTPUT_NAME chronolog-test-client-login-name)
gtest_discover_tests(chronolog_client_login_name_test TEST_PREFIX "Unit_ChronologClient_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronolog_client_login_name_test DESTINATION tests)
endif()

# Durability error codes
add_executable(chronolog_client_errcode_durability_test
    client_errcode_durability_test.cpp
)

target_include_directories(chronolog_client_errcode_durability_test PRIVATE
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
)

target_link_libraries(chronolog_client_errcode_durability_test PRIVATE
    GTest::gtest_main
    chronolog_client
)

set_target_properties(chronolog_client_errcode_durability_test PROPERTIES OUTPUT_NAME chronolog-test-client-errcode-durability)
gtest_discover_tests(chronolog_client_errcode_durability_test TEST_PREFIX "Unit_ChronologClient_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronolog_client_errcode_durability_test DESTINATION tests)
endif()

# StoryHandle::log_event(record, options, event_time) and LogEventOptions
add_executable(chronolog_client_log_event_options_test
    client_log_event_options_test.cpp
)

target_include_directories(chronolog_client_log_event_options_test PRIVATE
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(chronolog_client_log_event_options_test PRIVATE
    GTest::gtest_main
    chronolog_client
)

set_target_properties(chronolog_client_log_event_options_test PROPERTIES OUTPUT_NAME chronolog-test-client-log-event-options)
gtest_discover_tests(chronolog_client_log_event_options_test TEST_PREFIX "Unit_ChronologClient_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronolog_client_log_event_options_test DESTINATION tests)
endif()
