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
