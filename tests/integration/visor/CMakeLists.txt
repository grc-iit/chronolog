add_executable(clock_skew_probe clock_skew_probe.cpp)
target_link_libraries(clock_skew_probe PRIVATE chronolog::client chronolog_internal_proto chronolog_proto chronolog_warnings)
add_test(NAME visor.clock_skew
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${PROJECT_SOURCE_DIR}/tests/end-to-end/clock-skew/clock_skew_harness.sh"
    $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:clock_skew_probe>)
set_tests_properties(visor.clock_skew PROPERTIES TIMEOUT 240 LABELS integration)
