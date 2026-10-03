set(viz_core)
if(TARGET _core)
    set(viz_core $<TARGET_FILE:_core>)
endif()
add_custom_command(OUTPUT "${PROJECT_SOURCE_DIR}/build/viz/.built"
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/build.sh" ${viz_core}
    COMMAND "${CMAKE_COMMAND}" -E touch "${PROJECT_SOURCE_DIR}/build/viz/.built"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DEPENDS ${viz_core} build.sh prepare.sh
        backend/pyproject.toml backend/chronolog_viz.py
        grafana/package.json grafana/tsconfig.json grafana/webpack.config.cjs grafana/jest.config.cjs
        grafana/module.ts grafana/datasource.ts grafana/frames.ts grafana/frames.test.ts
        grafana/QueryEditor.tsx grafana/ConfigEditor.tsx grafana/types.ts grafana/plugin.json grafana/logo.svg)
add_custom_target(chronolog_viz ALL DEPENDS "${PROJECT_SOURCE_DIR}/build/viz/.built")
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
    set_tests_properties(plugin.viz.acceptance PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 120)
endif()
