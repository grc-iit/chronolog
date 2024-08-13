cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(MPI REQUIRED)

#message("CommunicationTest CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("CommunicationTest CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Build Target: CommunicationTest")

#set(CMAKE_FIND_DEBUG_MODE FALSE)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message("Compiling communication test in Debug Mode")
    #    add_compile_options(-g3)
    add_compile_options(-O0)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0")
endif()

add_executable(thallium_server
    ${CMAKE_CURRENT_SOURCE_DIR}/thallium_server.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp)
target_include_directories(thallium_server PRIVATE
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include)
target_link_libraries(thallium_server PRIVATE thallium)
target_link_libraries(thallium_server PUBLIC "-pthread")
add_test(NAME CommThalliumServer COMMAND thallium_server)

add_executable(thallium_client
    ${CMAKE_CURRENT_SOURCE_DIR}/thallium_client.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp)
target_include_directories(thallium_client PRIVATE
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include)
target_include_directories(thallium_client PRIVATE)
target_link_libraries(thallium_client thallium)
add_test(NAME CommThalliumClient COMMAND thallium_client)

add_executable(thallium_client_mpi
    ${CMAKE_CURRENT_SOURCE_DIR}/thallium_client_mpi.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp)
include_directories(SYSTEM ${MPI_INCLUDE_PATH})
target_include_directories(thallium_client_mpi PRIVATE
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include)
target_link_libraries(thallium_client_mpi thallium)
target_link_libraries(thallium_client_mpi ${MPI_C_LIBRARIES} ${MPI_CXX_LIBRARIES})
add_test(NAME CommThalliumClientMPI COMMAND thallium_client_mpi)
