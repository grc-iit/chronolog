add_executable(clock_skew_probe clock_skew_probe.cpp)
target_link_libraries(clock_skew_probe PRIVATE chronolog::client chronolog_internal_proto chronolog_proto chronolog_warnings)
add_test(NAME visor.clock_skew
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${PROJECT_SOURCE_DIR}/tests/end-to-end/clock-skew/clock_skew_harness.sh"
    $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:clock_skew_probe>)
set_tests_properties(visor.clock_skew PROPERTIES TIMEOUT 240 LABELS integration)
add_test(NAME visor.restart
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${PROJECT_SOURCE_DIR}/tests/integration/visor_restart_test.sh"
    $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_grapher>
    $<TARGET_FILE:chrono_player> $<TARGET_FILE:dynamic_rpc>)
set_tests_properties(visor.restart PROPERTIES TIMEOUT 330 LABELS integration)
