option(CHRONOLOG_BUILD_PLUGINS "Build ChronoLog plugins" ON)
find_package(Python3 3.12 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(chronolog_mcp
        COMMAND timeout 180 "${Python3_EXECUTABLE}" -m build --wheel --no-isolation
            --outdir "${PROJECT_BINARY_DIR}/smoke/wheels" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(CHRONOLOG_BUILD_PYTHON AND TARGET python_package AND TARGET chrono_visor AND TARGET chrono_keeper AND TARGET chrono_player)
        # One stack per file: the acceptance script bounds each pytest run to 60 s.
        foreach(suite tools recovery takeover bounds)
            add_test(NAME plugin.mcp.${suite} COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${PROJECT_SOURCE_DIR}/client/python/tests/acceptance.sh"
                $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_player>
                "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${suite}.py")
            set_tests_properties(plugin.mcp.${suite} PROPERTIES TIMEOUT 90
                ENVIRONMENT "PYTHONPATH=${PROJECT_BINARY_DIR}/client/python/binding/package:${CMAKE_CURRENT_SOURCE_DIR}/src")
        endforeach()
    endif()
endif()
