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
