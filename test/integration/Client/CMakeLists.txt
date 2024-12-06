cmake_minimum_required(VERSION 3.25)

find_package(OpenMP)
find_package(MPI)

#message("ClientExamples CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("ClientExamples CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Build target: ClientExamples")

set(client_examples client_lib_connect_rpc_test client_lib_metadata_rpc_test client_lib_multi_argobots_test client_lib_multi_pthread_test client_lib_multi_storytellers client_lib_thread_interdependency_test)

# Custom target to copy server_list.in file
add_custom_target(copyserverlist ALL
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_SOURCE_DIR}/test/integration/server_list.in
    ${CMAKE_CURRENT_BINARY_DIR}/server_list
)

foreach(client ${client_examples})
    add_executable(${client} ${CMAKE_CURRENT_SOURCE_DIR}/${client}.cpp)
    add_dependencies(${client} copyserverlist)
    target_include_directories(${client} PRIVATE ${CMAKE_BINARY_DIR}/Client/include)
    target_link_libraries(${client} chronolog_client -lpthread -lrt)
    #add_test(NAME ${client} COMMAND ${client})
endforeach()

set(client_openmp client_lib_multi_openmp_test)

foreach(client ${client_openmp})
    add_executable(${client} ${CMAKE_CURRENT_SOURCE_DIR}/${client}.cpp)
    add_dependencies(${client} copyserverlist)
    target_include_directories(${client} PRIVATE ${CMAKE_BINARY_DIR}/Client/include)
    target_link_libraries(${client} chronolog_client OpenMP::OpenMP_CXX)
endforeach()

set(client_mpi client_lib_hybrid_argobots_test)

foreach(client ${client_mpi})
    add_executable(${client} ${CMAKE_CURRENT_SOURCE_DIR}/${client}.cpp)
    add_dependencies(${client} copyserverlist)
    target_include_directories(${client} PRIVATE ${CMAKE_BINARY_DIR}/Client/include ${MPI_CXX_INCLUDE_DIRS})
    target_link_libraries(${client} chronolog_client ${MPI_CXX_LIBRARIES} -lpthread -lrt)
    configure_file(${CMAKE_SOURCE_DIR}/default_conf.json.in
        ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json COPYONLY)
endforeach()

# install binaries
install(
    TARGETS ${client_examples} DESTINATION bin
)

# install configuration file
install(
    FILES ${CMAKE_SOURCE_DIR}/default_conf.json.in DESTINATION ${CHRONOLOG_INSTALL_CONF_DIR} RENAME default_conf.json
)
