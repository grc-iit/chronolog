find_package(mercury REQUIRED)
find_package(thallium REQUIRED)
find_package(MPI REQUIRED)
find_package(OpenMP)

set(client_examples client_lib_connect_rpc_test client_lib_metadata_rpc_test client_lib_multi_argobots_test client_lib_multi_pthread_test)

add_custom_target(copyserverlist)

add_custom_command(
        TARGET copyserverlist
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/../server_list.in
        ${CMAKE_CURRENT_BINARY_DIR}/server_list)


foreach(client ${client_examples})
    add_executable(${client} ${client}.cpp)
    add_dependencies(${client} copyserverlist)
    target_include_directories(${client} PRIVATE ../Client/include)
    target_link_libraries(${client} ChronoLogClient_lib -lpthread -lrt)
    #add_test(NAME ${client} COMMAND ${client})
endforeach()

set(client_openmp client_lib_multi_openmp_test)

foreach(client ${client_openmp})
   add_executable(${client} ${client}.cpp)
   add_dependencies(${client} copyserverlist)
   target_include_directories(${client} PRIVATE ../Client/include)
   target_link_libraries(${client} ChronoLogClient_lib OpenMP::OpenMP_CXX)
endforeach()   

set(client_mpi client_lib_hybrid_argobots_test)

message(${MPI_CXX_INCLUDE_DIRS})
message(${MPI_CXX_LIBRARIES})

foreach(client ${client_mpi})
	add_executable(${client} ${client}.cpp)
	add_dependencies(${client} copyserverlist)
	target_include_directories(${client} PRIVATE ../Client/include ${MPI_CXX_INCLUDE_DIRS})
	target_link_libraries(${client} ChronoLogClient_lib ${MPI_CXX_LIBRARIES} -lpthread -lrt)
endforeach()

