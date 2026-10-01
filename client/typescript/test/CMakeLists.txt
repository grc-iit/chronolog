if(TARGET chrono_visor AND TARGET chrono_keeper AND TARGET chrono_player)
    add_test(NAME typescript.acceptance COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_stack.sh"
        $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_player>)
    set_tests_properties(typescript.acceptance PROPERTIES TIMEOUT 100 SKIP_RETURN_CODE 77)
endif()
