find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
add_executable(local_driver tests/driver.cpp)
target_link_libraries(local_driver PRIVATE chronolog::client chronolog_warnings)
foreach(gate readiness restart_keeps_endpoints single_supervisor ordered_stop supervisor_death attach_leases bind_guard)
    add_test(NAME local.${gate}
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gates.py" ${gate}
        "${CMAKE_CURRENT_SOURCE_DIR}/chronolog" "${PROJECT_BINARY_DIR}"
        $<TARGET_FILE:local_driver>)
    set_tests_properties(local.${gate} PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 240 LABELS "integration;local")
endforeach()

foreach(gate tier_add_refuses_a_root_of_another_file_system_type status_survives_a_hung_tier)
    add_test(NAME local.${gate}
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/tiers.py" ${gate}
        "${CMAKE_CURRENT_SOURCE_DIR}/chronolog" "${PROJECT_BINARY_DIR}")
    set_tests_properties(local.${gate} PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 120 LABELS "integration;local")
endforeach()

if(NOT CHRONOLOG_BUILD_PYTHON AND NOT CHRONOLOG_SANITIZERS)
    set(local_wheel_dir "${CMAKE_CURRENT_BINARY_DIR}/wheels")
    add_custom_target(chronolog_local_wheels ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${local_wheel_dir}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_wheel.py"
            "${local_wheel_dir}"
        VERBATIM)
    add_test(NAME server.install
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/install.py" "${PROJECT_BINARY_DIR}"
            "${local_wheel_dir}" "${CMAKE_INSTALL_BINDIR}")
    set_tests_properties(server.install PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 1200 SKIP_RETURN_CODE 77 LABELS "integration;install")
    add_test(NAME local.external_liveness
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/external.py" "${local_wheel_dir}"
            "${CMAKE_CURRENT_SOURCE_DIR}/chronolog" $<TARGET_FILE:chrono_visor>)
    set_tests_properties(local.external_liveness PROPERTIES RESOURCE_LOCK chronolog_stack TIMEOUT 1200 SKIP_RETURN_CODE 77 LABELS "integration;local")
endif()
