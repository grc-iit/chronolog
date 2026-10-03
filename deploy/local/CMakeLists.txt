find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
add_executable(local_driver tests/driver.cpp)
target_link_libraries(local_driver PRIVATE chronolog::client chronolog_warnings)
foreach(gate readiness restart_keeps_endpoints single_supervisor ordered_stop supervisor_death attach_leases bind_guard)
    add_test(NAME local.${gate}
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gates.py" ${gate}
        "${CMAKE_CURRENT_SOURCE_DIR}/chronolog" "${PROJECT_BINARY_DIR}"
        $<TARGET_FILE:local_driver>)
    set_tests_properties(local.${gate} PROPERTIES TIMEOUT 240 LABELS "integration;local")
endforeach()

if(NOT CHRONOLOG_BUILD_PYTHON AND NOT CHRONOLOG_SANITIZERS)
    set(local_wheel_dir "${CMAKE_CURRENT_BINARY_DIR}/wheels")
    add_custom_target(chronolog_local_wheels ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${local_wheel_dir}"
        COMMAND timeout 180 "${Python3_EXECUTABLE}" -m build --wheel --no-isolation
            --outdir "${local_wheel_dir}" "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMAND timeout 900 "${Python3_EXECUTABLE}" -m build --wheel --no-isolation
            --outdir "${local_wheel_dir}" "${PROJECT_SOURCE_DIR}/client/python"
        VERBATIM)
    add_test(NAME server.install
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/install.py" "${PROJECT_BINARY_DIR}"
            "${local_wheel_dir}" "${CMAKE_INSTALL_BINDIR}")
    set_tests_properties(server.install PROPERTIES TIMEOUT 300 LABELS "integration;install")
endif()
