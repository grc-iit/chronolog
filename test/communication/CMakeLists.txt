cmake_minimum_required(VERSION 3.19)

find_package(Thallium REQUIRED)
find_package(MPI REQUIRED)

#set(CMAKE_FIND_DEBUG_MODE FALSE)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message("Compiling communication test in Debug Mode")
    #    add_compile_options(-g3)
    add_compile_options(-O0)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0")
endif()

add_executable(thallium_server thallium_server.cpp
    ../../ChronoAPI/ChronoLog/src/log.cpp)
target_include_directories(thallium_server PRIVATE
    ../../ChronoAPI/ChronoLog/include)
target_link_libraries(thallium_server PRIVATE thallium)
target_link_libraries(thallium_server PUBLIC "-pthread")
add_test(NAME CommThalliumServer COMMAND thallium_server)

add_executable(thallium_client thallium_client.cpp
    ../../ChronoAPI/ChronoLog/src/log.cpp)
target_include_directories(thallium_client PRIVATE
    ../../ChronoAPI/ChronoLog/include)
target_include_directories(thallium_client PRIVATE)
target_link_libraries(thallium_client thallium)
add_test(NAME CommThalliumClient COMMAND thallium_client)

add_executable(thallium_client_mpi thallium_client_mpi.cpp
    ../../ChronoAPI/ChronoLog/src/log.cpp)
include_directories(SYSTEM ${MPI_INCLUDE_PATH})
target_include_directories(thallium_client_mpi PRIVATE
    ../../ChronoAPI/ChronoLog/include)
target_link_libraries(thallium_client_mpi thallium)
target_link_libraries(thallium_client_mpi ${MPI_C_LIBRARIES} ${MPI_CXX_LIBRARIES})
add_test(NAME CommThalliumClientMPI COMMAND thallium_client_mpi)
