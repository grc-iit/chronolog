find_package(Python 3.12 COMPONENTS Interpreter Development.Module Development.SABIModule REQUIRED)
execute_process(COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_VARIABLE nanobind_DIR OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
find_package(nanobind CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# The Context layer is compiled in so the module stays self-contained over the static SDK.
set(context_dir "${PROJECT_SOURCE_DIR}/client/cpp/context")
nanobind_add_module(_core STABLE_ABI NB_STATIC core.cpp context.cpp
    "${context_dir}/context.cpp" "${context_dir}/recall.cpp" "${context_dir}/follow.cpp"
    "${context_dir}/latest.cpp" "${context_dir}/reconcile.cpp")
target_include_directories(_core PRIVATE "${context_dir}/include")
target_link_libraries(_core PRIVATE chronolog::client_static chronolog_proto nlohmann_json::nlohmann_json OpenSSL::Crypto)
install(TARGETS _core LIBRARY DESTINATION chronolog COMPONENT python)

# Tests import the package staged next to the module just built, not an installed wheel.
set(package_dir "${CMAKE_CURRENT_BINARY_DIR}/package")
add_custom_target(python_package ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../chronolog" "${package_dir}/chronolog"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:_core>" "${package_dir}/chronolog/"
    DEPENDS _core VERBATIM)
set(python_env "PYTHONPATH=${package_dir}")
add_test(NAME python.values COMMAND "${Python_EXECUTABLE}" -m pytest -q "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_values.py")
set_tests_properties(python.values PROPERTIES TIMEOUT 30 ENVIRONMENT "${python_env}")
if(TARGET chrono_visor AND TARGET chrono_keeper AND TARGET chrono_player)
    foreach(suite acceptance:test_stack otel:test_otel context:test_context)
        string(REPLACE ":" ";" parts "${suite}")
        list(GET parts 0 name)
        list(GET parts 1 file)
        add_test(NAME python.${name} COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${CMAKE_CURRENT_SOURCE_DIR}/../tests/acceptance.sh"
            $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_player>
            "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tests/${file}.py")
        set_tests_properties(python.${name} PROPERTIES TIMEOUT 90 ENVIRONMENT "${python_env}")
    endforeach()
endif()
