find_package(Python 3.12 COMPONENTS Interpreter Development.Module Development.SABIModule REQUIRED)
execute_process(COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_VARIABLE nanobind_DIR OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
find_package(nanobind CONFIG REQUIRED)
nanobind_add_module(_core STABLE_ABI NB_STATIC core.cpp)
target_link_libraries(_core PRIVATE chronolog::client_static)
install(TARGETS _core LIBRARY DESTINATION chronolog COMPONENT python)
add_test(NAME python.values COMMAND "${Python_EXECUTABLE}" -m pytest -q "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_values.py")
set_tests_properties(python.values PROPERTIES TIMEOUT 30)
if(TARGET chrono_visor AND TARGET chrono_keeper AND TARGET chrono_player)
    add_test(NAME python.acceptance COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${CMAKE_CURRENT_SOURCE_DIR}/../tests/acceptance.sh"
        $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_player>
        "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_stack.py")
    set_tests_properties(python.acceptance PROPERTIES TIMEOUT 90)
endif()

if(TARGET chrono_visor AND TARGET chrono_keeper AND TARGET chrono_player)
    add_test(NAME python.otel COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${CMAKE_CURRENT_SOURCE_DIR}/../tests/acceptance.sh"
        $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_player>
        "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_otel.py")
    set_tests_properties(python.otel PROPERTIES TIMEOUT 90)
endif()
