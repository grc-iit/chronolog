cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(MPI REQUIRED)

#------------------------------------------------------------------------------
# Build Configuration
#------------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Compiling communication test in Debug Mode")
    add_compile_options(-O0)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0")
endif()

#------------------------------------------------------------------------------
# Test Executables
#------------------------------------------------------------------------------

# Thallium Server Test
add_executable(thallium_server
    thallium_server.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_include_directories(thallium_server PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_libraries(thallium_server
    PRIVATE
        thallium
        pthread
)

add_test(NAME CommThalliumServer COMMAND thallium_server)

# Thallium Client Test
add_executable(thallium_client
    thallium_client.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_include_directories(thallium_client PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_libraries(thallium_client
    PRIVATE
        thallium
)

add_test(NAME CommThalliumClient COMMAND thallium_client)

# Thallium Client MPI Test
add_executable(thallium_client_mpi
    thallium_client_mpi.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_include_directories(thallium_client_mpi SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${MPI_INCLUDE_PATH}
)

target_link_libraries(thallium_client_mpi
    PRIVATE
        thallium
        ${MPI_C_LIBRARIES}
        ${MPI_CXX_LIBRARIES}
)

add_test(NAME CommThalliumClientMPI COMMAND thallium_client_mpi)
