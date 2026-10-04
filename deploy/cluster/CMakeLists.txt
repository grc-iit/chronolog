add_executable(cluster_manifest_probe manifest_probe.cpp)
target_link_libraries(cluster_manifest_probe PRIVATE chronolog_common_tier chronolog_warnings)

find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
foreach(gate homelab_keeps_the_sprint_placement example_lab_resolves_hosts_from_the_file
        grapher_order_follows_the_file invalid_labs_are_refused code_names_no_lab_host)
    add_test(NAME cluster.${gate}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/config.py" ${gate})
    set_tests_properties(cluster.${gate} PROPERTIES TIMEOUT 30 LABELS cluster)
endforeach()
