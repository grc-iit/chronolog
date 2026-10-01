set(viz_core)
if(TARGET _core)
    set(viz_core $<TARGET_FILE:_core>)
endif()
add_custom_target(chronolog_viz
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/build.sh" ${viz_core}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
if(TARGET _core)
    add_dependencies(chronolog_viz _core)
endif()
add_test(NAME plugin.viz.frontend COMMAND timeout 120 npm test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/build/viz")
set_tests_properties(plugin.viz.frontend PROPERTIES TIMEOUT 130)
if(TARGET _core AND TARGET chrono_visor AND TARGET chrono_keeper AND TARGET chrono_player)
    add_test(NAME plugin.viz.acceptance COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
        bash "${CMAKE_CURRENT_SOURCE_DIR}/backend/acceptance.sh"
        $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_player>
        "${PROJECT_SOURCE_DIR}/build/viz-venv/bin/python"
        "${CMAKE_CURRENT_SOURCE_DIR}/backend/test_stack.py")
    set_tests_properties(plugin.viz.acceptance PROPERTIES TIMEOUT 120)
endif()
