cmake_minimum_required(VERSION 3.25)

find_package(OpenMP)
find_package(MPI)

# Option to install test binaries
option(CHRONOLOG_INSTALL_TESTS "Install test binaries" OFF)

#message("ClientExamples CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("ClientExamples CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Build target: ClientExamples")

set(client_examples 
    client_lib_connect_rpc_test client_lib_metadata_rpc_test 
    client_lib_multi_argobots_test client_lib_multi_pthread_test  
    client_lib_thread_interdependency_test
    client_lib_multi_storytellers 
    client_lib_story_reader
    client_reader_to_csv
    )

foreach(client ${client_examples})
    add_executable(${client} ${client}.cpp)
    target_include_directories(${client} PRIVATE ${CMAKE_BINARY_DIR}/Client/include)
    target_link_libraries(${client} chronolog_client -lpthread -lrt)
    set_target_properties(${client} PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        SKIP_BUILD_RPATH TRUE
    )
endforeach()

set(client_openmp client_lib_multi_openmp_test)

foreach(client ${client_openmp})
    add_executable(${client} ${client}.cpp)
    target_include_directories(${client} PRIVATE ${CMAKE_BINARY_DIR}/Client/include)
    target_link_libraries(${client} chronolog_client OpenMP::OpenMP_CXX)
    set_target_properties(${client} PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        SKIP_BUILD_RPATH TRUE
    )
endforeach()

set(client_mpi client_lib_hybrid_argobots_test)

foreach(client ${client_mpi})
    add_executable(${client} ${client}.cpp)
    target_include_directories(${client} PRIVATE ${CMAKE_BINARY_DIR}/Client/include ${MPI_CXX_INCLUDE_DIRS})
    target_link_libraries(${client} chronolog_client ${MPI_CXX_LIBRARIES} -lpthread -lrt)
    set_target_properties(${client} PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        SKIP_BUILD_RPATH TRUE
    )
endforeach()

# install binaries (only if tests are enabled)
if(CHRONOLOG_INSTALL_TESTS)
    foreach(client ${client_examples})
        chronolog_install_target(${client})
    endforeach()
    
endif()
