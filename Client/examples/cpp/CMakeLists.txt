cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(MPI REQUIRED)

#------------------------------------------------------------------------------
# Example Executables
#------------------------------------------------------------------------------

# Writer example
add_executable(client_lib_writer_example
    client_lib_writer_example.cpp
)

target_include_directories(client_lib_writer_example PRIVATE
    ${CMAKE_BINARY_DIR}/Client/cpp/include
)

target_link_libraries(client_lib_writer_example
    PRIVATE
        chronolog_client
        pthread
        rt
)

# Reader example
add_executable(client_lib_reader_example
    client_lib_reader_example.cpp
)

target_include_directories(client_lib_reader_example PRIVATE
    ${CMAKE_BINARY_DIR}/Client/cpp/include
)

target_link_libraries(client_lib_reader_example
    PRIVATE
        chronolog_client
        pthread
        rt
)

# Basic (read-write) example
add_executable(client_lib_basic_example
    client_lib_basic_example.cpp
)

target_include_directories(client_lib_basic_example PRIVATE
    ${CMAKE_BINARY_DIR}/Client/cpp/include
)

target_link_libraries(client_lib_basic_example
    PRIVATE
        chronolog_client
        pthread
        rt
)

# Distributed telemetry writer example
# Note: margo.h is included directly, so we need thallium includes
add_executable(client_lib_distributed_telemetry_writer
    client_lib_distributed_telemetry_writer.cpp
)

target_include_directories(client_lib_distributed_telemetry_writer PRIVATE
    ${CMAKE_BINARY_DIR}/Client/cpp/include
)

target_link_libraries(client_lib_distributed_telemetry_writer
    PRIVATE
        chronolog_client
        thallium
        pthread
        rt
        ${MPI_CXX_LIBRARIES}
)

#------------------------------------------------------------------------------
# Install example binaries
#------------------------------------------------------------------------------
chronolog_install_target(client_lib_writer_example)
chronolog_install_target(client_lib_reader_example)
chronolog_install_target(client_lib_basic_example)
chronolog_install_target(client_lib_distributed_telemetry_writer)

# Client configuration file is installed by Client/cpp to avoid duplication
