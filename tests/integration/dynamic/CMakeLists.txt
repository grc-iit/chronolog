add_executable(dynamic_rpc rpc.cpp)
target_link_libraries(dynamic_rpc PRIVATE chronolog_proto chronolog_internal_proto nlohmann_json::nlohmann_json chronolog_warnings)
add_test(NAME visor.dynamic_failover
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} timeout -k 15 360 python3
    ${CMAKE_CURRENT_SOURCE_DIR}/run.py
    --rpc $<TARGET_FILE:dynamic_rpc>
    --visor $<TARGET_FILE:chrono_visor>
    --keeper $<TARGET_FILE:chrono_keeper>
    --grapher $<TARGET_FILE:chrono_grapher>
    --player $<TARGET_FILE:chrono_player>)
set_tests_properties(visor.dynamic_failover PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 380)
add_test(NAME visor.dynamic_physical_claim
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} timeout -k 15 300 python3
    ${CMAKE_CURRENT_SOURCE_DIR}/run.py --scenario physical-claim
    --rpc $<TARGET_FILE:dynamic_rpc>
    --visor $<TARGET_FILE:chrono_visor>
    --keeper $<TARGET_FILE:chrono_keeper>
    --grapher $<TARGET_FILE:chrono_grapher>
    --player $<TARGET_FILE:chrono_player>)
set_tests_properties(visor.dynamic_physical_claim PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 320)
add_test(NAME visor.dynamic_prefix_claim
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} timeout -k 15 300 python3
    ${CMAKE_CURRENT_SOURCE_DIR}/run.py --scenario prefix-claim
    --rpc $<TARGET_FILE:dynamic_rpc>
    --visor $<TARGET_FILE:chrono_visor>
    --keeper $<TARGET_FILE:chrono_keeper>
    --grapher $<TARGET_FILE:chrono_grapher>
    --player $<TARGET_FILE:chrono_player>)
set_tests_properties(visor.dynamic_prefix_claim PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 320)
add_test(NAME visor.dynamic_scenario
    COMMAND timeout 30 python3 ${CMAKE_CURRENT_SOURCE_DIR}/scenario_test.py)
set_tests_properties(visor.dynamic_scenario PROPERTIES TIMEOUT 35)
add_executable(dynamic_bootstrap_test bootstrap_test.cpp)
target_link_libraries(dynamic_bootstrap_test PRIVATE chronolog_visor_lib GTest::gtest_main chronolog_warnings)
include(GoogleTest)
gtest_discover_tests(dynamic_bootstrap_test DISCOVERY_TIMEOUT 60 TEST_PREFIX "visor." DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
