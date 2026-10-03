add_executable(keeper_wal_driver driver.cpp)
target_link_libraries(keeper_wal_driver PRIVATE chronolog::client chronolog_internal_proto chronolog_proto chronolog_warnings)
add_test(NAME keeper.wal_restart
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${PROJECT_SOURCE_DIR}/tests/integration/keeper_wal_restart_test.sh"
    $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_grapher> $<TARGET_FILE:chrono_player>
    $<TARGET_FILE:keeper_wal_driver>)
set_tests_properties(keeper.wal_restart PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 240 LABELS integration)
add_test(NAME grapher.manifest_restart
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${PROJECT_SOURCE_DIR}/tests/integration/manifest_restart_test.sh"
    $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_grapher> $<TARGET_FILE:chrono_player>
    $<TARGET_FILE:keeper_wal_driver>)
set_tests_properties(grapher.manifest_restart PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 240 LABELS integration)
add_library(compaction_barrier SHARED compaction_barrier.cpp)
target_link_libraries(compaction_barrier PRIVATE ${CMAKE_DL_LIBS} chronolog_warnings)
add_test(NAME grapher.compaction
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${PROJECT_SOURCE_DIR}/tests/integration/compaction_test.sh"
    $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_grapher> $<TARGET_FILE:chrono_player>
    $<TARGET_FILE:keeper_wal_driver> $<TARGET_FILE:compaction_barrier>)
set_tests_properties(grapher.compaction PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 240 LABELS integration)
