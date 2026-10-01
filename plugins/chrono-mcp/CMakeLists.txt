option(CHRONOLOG_BUILD_PLUGINS "Build ChronoLog plugins" ON)
find_package(Python3 3.12 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(chronolog_mcp
        COMMAND timeout 180 "${Python3_EXECUTABLE}" -m build --wheel --no-isolation
            --outdir "${PROJECT_BINARY_DIR}/smoke/wheels" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(CHRONOLOG_BUILD_PYTHON AND TARGET chrono_visor AND TARGET chrono_keeper AND TARGET chrono_player)
        add_test(NAME plugin.mcp COMMAND bash "${PROJECT_SOURCE_DIR}/client/python/tests/acceptance.sh"
            $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_player>
            "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        set_tests_properties(plugin.mcp PROPERTIES TIMEOUT 90)
    endif()
endif()
