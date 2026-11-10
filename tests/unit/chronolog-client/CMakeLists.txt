cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
find_package(Thallium REQUIRED)
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

add_executable(chronolog_rpc_policy_test rpc_policy_test.cpp)
target_include_directories(chronolog_rpc_policy_test PRIVATE ${CMAKE_SOURCE_DIR}/client/cpp/include ${CMAKE_SOURCE_DIR}/client/cpp/src)
target_link_libraries(chronolog_rpc_policy_test PRIVATE GTest::gtest_main chronolog_client)
gtest_discover_tests(chronolog_rpc_policy_test TEST_PREFIX "Unit_ChronologRpc_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronolog_rpc_policy_test DESTINATION tests)
endif()

add_executable(chronolog_timer_test chronolog_timer_test.cpp)
target_include_directories(chronolog_timer_test PRIVATE ${CMAKE_SOURCE_DIR}/client/cpp/include ${CMAKE_SOURCE_DIR}/client/cpp/src)
target_link_libraries(chronolog_timer_test PRIVATE GTest::gtest_main chronolog_client chrono_rpc thallium)
set_target_properties(chronolog_timer_test PROPERTIES OUTPUT_NAME chronolog-test-client-timer)
gtest_discover_tests(chronolog_timer_test TEST_PREFIX "Unit_ChronologClient_" PROPERTIES TIMEOUT 30)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronolog_timer_test DESTINATION tests)
endif()

add_executable(metadata_operation_state_test metadata_operation_state_test.cpp)
target_include_directories(metadata_operation_state_test PRIVATE ${CMAKE_SOURCE_DIR}/client/cpp/src)
target_link_libraries(metadata_operation_state_test PRIVATE GTest::gtest_main chronolog_client)
gtest_discover_tests(metadata_operation_state_test TEST_PREFIX "Unit_ChronologClient_")

# Native peers live in a child so the real client owns cold ABT startup.
add_executable(client_native_peer ${CMAKE_SOURCE_DIR}/tests/support/client_native_peer.cpp)
target_include_directories(client_native_peer PRIVATE ${CMAKE_SOURCE_DIR}/tests/support ${CMAKE_SOURCE_DIR}/src/chrono-common/include ${CMAKE_SOURCE_DIR}/client/cpp/include)
target_compile_definitions(client_native_peer PRIVATE CHRONOLOG_NATIVE_PEER_PATH="unused")
target_link_libraries(client_native_peer PRIVATE chronolog_client thallium)
target_include_directories(chronolog_timer_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)
target_compile_definitions(chronolog_timer_test PRIVATE CHRONOLOG_NATIVE_PEER_PATH="$<TARGET_FILE:client_native_peer>")
add_dependencies(chronolog_timer_test client_native_peer)

add_executable(append_deadline_test append_deadline_test.cpp)
target_link_libraries(append_deadline_test PRIVATE GTest::gtest_main chronolog_client)
gtest_discover_tests(append_deadline_test TEST_PREFIX "Unit_ChronologClient_")

add_executable(tail_deadline_test tail_deadline_test.cpp)
target_include_directories(tail_deadline_test PRIVATE ${CMAKE_SOURCE_DIR}/client/cpp/src ${CMAKE_SOURCE_DIR}/tests/support)
target_compile_definitions(tail_deadline_test PRIVATE CHRONOLOG_NATIVE_PEER_PATH="$<TARGET_FILE:client_native_peer>")
target_link_libraries(tail_deadline_test PRIVATE GTest::gtest_main chronolog_client chrono_rpc thallium)
add_dependencies(tail_deadline_test client_native_peer)
# This fixture and its child native peer bind only 127.0.0.1.
gtest_discover_tests(tail_deadline_test TEST_PREFIX "Unit_ChronologClient_"
    PROPERTIES TIMEOUT 30 ENVIRONMENT "FI_TCP_IFACE=lo")

add_executable(replay_deadline_test replay_deadline_test.cpp)
target_include_directories(replay_deadline_test PRIVATE ${CMAKE_SOURCE_DIR}/client/cpp/src)
target_link_libraries(replay_deadline_test PRIVATE GTest::gtest_main chronolog_client chrono_rpc thallium)
gtest_discover_tests(replay_deadline_test TEST_PREFIX "Unit_ChronologClient_" PROPERTIES TIMEOUT 30)

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

# StoryHandle::log_event(record, options, event_time) through the owned RPC
# runtime, against a keeper in the child native peer.
add_executable(durable_deadline_test durable_deadline_test.cpp)
target_include_directories(durable_deadline_test PRIVATE ${CMAKE_SOURCE_DIR}/client/cpp/src ${CMAKE_SOURCE_DIR}/tests/support)
target_compile_definitions(durable_deadline_test PRIVATE CHRONOLOG_NATIVE_PEER_PATH="$<TARGET_FILE:client_native_peer>")
target_link_libraries(durable_deadline_test PRIVATE GTest::gtest_main chronolog_client chrono_rpc thallium)
add_dependencies(durable_deadline_test client_native_peer)
# This fixture and its child native peer bind only 127.0.0.1.
gtest_discover_tests(durable_deadline_test TEST_PREFIX "Unit_ChronologClient_"
    PROPERTIES TIMEOUT 30 ENVIRONMENT "FI_TCP_IFACE=lo")

# Python binding of Durability, LogEventOptions and log_event(record, options)
if(TARGET py_chronolog_client)
    find_package(Python COMPONENTS Interpreter REQUIRED)
    add_test(NAME Unit_PyClient_DurableBinding
        COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/py_client_durable_binding_test.py)
    set_tests_properties(Unit_PyClient_DurableBinding PROPERTIES
        ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:py_chronolog_client>")
endif()
