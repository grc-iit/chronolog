# target_link_libraries(DistributedHashMapTest ${CMAKE_BINARY_DIR}/libhcl.so)

set(examples unordered_map_test unordered_map_string_test map_test queue_test priority_queue_test multimap_test set_test global_clock_test)

add_custom_target(copy_hostfile)
add_custom_command(TARGET copy_hostfile
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/hostfile
        ${CMAKE_CURRENT_BINARY_DIR})

add_custom_target(copy_server_list)
add_custom_command(
        TARGET copy_server_list
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/server_list
        ${CMAKE_CURRENT_BINARY_DIR})

# Compile all examples
foreach (example ${examples})
    add_executable(${example} ${example}.cpp util.h)
    add_dependencies(${example} ${PROJECT_NAME})
    add_dependencies(${example} copy_hostfile)
    add_dependencies(${example} copy_server_list)
    set_target_properties(${example} PROPERTIES ENVIRONMENT LD_PRELOAD=${CMAKE_BINARY_DIR}/libhcl.so:${THALLIUM_LIBRARY_LD_PRELOAD})
    target_link_libraries(${example} ${PROJECT_NAME})
    target_link_libraries(${example} ${MPI_CXX_LIBRARIES})
    set_target_properties(${example} PROPERTIES FOLDER test)
endforeach ()

message(STATUS ${CMAKE_BINARY_DIR}/libhcl.so)

# Define MPI test case template
function(mpi target mpi_procs example ranks_per_process num_requests size_of_request server_on_node debug)
    set(test_parameters -np ${mpi_procs} -f "${CMAKE_BINARY_DIR}/test/hostfile" "${CMAKE_BINARY_DIR}/test/${example}" ${ranks_per_process} ${num_requests} ${size_of_request} ${server_on_node} ${debug})
    set(test_name ${target}_${example}_MPI_${mpi_procs}_${ranks_per_process}_${num_requests}_${size_of_request}_${server_on_node}_${debug})
    add_test(NAME ${test_name} COMMAND ${MPIEXEC_EXECUTABLE} ${test_parameters})
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_BINARY_DIR}/libhcl.so;LD_LIBRARY_PATH=${CMAKE_SOURCE_DIR}/.spack-env/view/lib:$ENV{LD_LIBRARY_PATH};SERVER_LIST_PATH=${CMAKE_BINARY_DIR}/test/")

endfunction()

# Define MPI test case
foreach (example ${examples})
    mpi(ares 4 ${example} 2 500 1000 0 0)
endforeach ()

set(examples unordered_map_test unordered_map_string_test map_test queue_test priority_queue_test multimap_test set_test)

foreach (example ${examples})
    mpi(ares 4 ${example} 2 500 1000 1 0)
endforeach ()

foreach (example ${examples})
    set(test_parameters -np 2 "${CMAKE_BINARY_DIR}/test/${example}")
    add_test(NAME ${example} COMMAND ${MPIEXEC_EXECUTABLE} ${test_parameters})
    set_tests_properties(${example} PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_BINARY_DIR}/libhcl.so;LD_LIBRARY_PATH=${CMAKE_SOURCE_DIR}/.spack-env/view/lib:$ENV{LD_LIBRARY_PATH};SERVER_LIST_PATH=${CMAKE_BINARY_DIR}/test/")
endforeach ()