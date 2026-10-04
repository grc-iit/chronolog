# Real-process integration tests over the four service binaries.
add_subdirectory(dynamic)
add_subdirectory(keeper_wal)
add_subdirectory(visor)
add_subdirectory(lease)
add_test(NAME integration.bind_guard
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash ${CMAKE_CURRENT_SOURCE_DIR}/bind_guard_test.sh
    $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_grapher>)
set_tests_properties(integration.bind_guard PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 180 LABELS "integration")
