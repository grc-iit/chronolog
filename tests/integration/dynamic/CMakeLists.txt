add_executable(dynamic_rpc rpc.cpp)
target_link_libraries(dynamic_rpc PRIVATE chronolog_proto chronolog_internal_proto nlohmann_json::nlohmann_json chronolog_warnings)
add_test(NAME dynamic_failover_integration
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} timeout -k 15 360 python3
    ${CMAKE_CURRENT_SOURCE_DIR}/run.py
    --rpc $<TARGET_FILE:dynamic_rpc>
    --visor $<TARGET_FILE:chrono_visor>
    --keeper $<TARGET_FILE:chrono_keeper>
    --grapher $<TARGET_FILE:chrono_grapher>
    --player $<TARGET_FILE:chrono_player>)
set_tests_properties(dynamic_failover_integration PROPERTIES TIMEOUT 380)
add_executable(dynamic_bootstrap_test bootstrap_test.cpp)
target_include_directories(dynamic_bootstrap_test PRIVATE ${PROJECT_SOURCE_DIR}/src/chrono-visor/tests)
target_link_libraries(dynamic_bootstrap_test PRIVATE chronolog_visor_lib GTest::gtest_main chronolog_warnings)
include(GoogleTest)
gtest_discover_tests(dynamic_bootstrap_test DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
