add_executable(lease_lifecycle_driver driver.cpp)
target_link_libraries(lease_lifecycle_driver PRIVATE chronolog::client chronolog_proto chronolog_warnings)
add_test(NAME lease.lifecycle
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${CMAKE_CURRENT_SOURCE_DIR}/run.sh"
    $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_grapher>
    $<TARGET_FILE:chrono_player> $<TARGET_FILE:lease_lifecycle_driver>)
set_tests_properties(lease.lifecycle PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 150 LABELS integration)
