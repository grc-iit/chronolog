find_package(GTest REQUIRED)
find_package(Thallium REQUIRED)
include(GoogleTest)

add_executable(rpc_deadline_test rpc_deadline_test.cpp)
target_link_libraries(rpc_deadline_test PRIVATE chrono_rpc GTest::gtest_main)
gtest_discover_tests(rpc_deadline_test TEST_PREFIX "Unit_ChronologRpc_" PROPERTIES TIMEOUT 20)

add_executable(rpc_runtime_test rpc_runtime_test.cpp)
target_link_libraries(rpc_runtime_test PRIVATE chrono_rpc GTest::gtest_main thallium)
gtest_discover_tests(rpc_runtime_test TEST_PREFIX "Unit_ChronologRpc_" PROPERTIES TIMEOUT 20)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(rpc_deadline_test DESTINATION tests)
    chronolog_install_target(rpc_runtime_test DESTINATION tests)
endif()

# Isolated native lifecycle feasibility only; each discovered case starts cold.
add_executable(rpc_native_cold_lifecycle_probe rpc_native_cold_lifecycle_probe.cpp)
target_link_libraries(rpc_native_cold_lifecycle_probe PRIVATE chrono_rpc GTest::gtest_main thallium)
gtest_discover_tests(rpc_native_cold_lifecycle_probe
    TEST_PREFIX "Integration_ChronologRpcColdLifecycle_" PROPERTIES TIMEOUT 30)

add_executable(native_runtime_test native_runtime_test.cpp)
target_link_libraries(native_runtime_test PRIVATE chrono_rpc GTest::gtest_main thallium)
gtest_discover_tests(native_runtime_test TEST_PREFIX "Integration_ChronologNativeRuntime_" PROPERTIES TIMEOUT 30)

add_executable(lifecycle_record_test lifecycle_record_test.cpp)
target_link_libraries(lifecycle_record_test PRIVATE chrono_rpc GTest::gtest_main)
gtest_discover_tests(lifecycle_record_test TEST_PREFIX "Unit_ChronologLifecycle_" PROPERTIES TIMEOUT 10)
