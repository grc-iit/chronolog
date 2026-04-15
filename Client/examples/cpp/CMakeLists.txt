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

set_target_properties(client_lib_writer_example PROPERTIES
    OUTPUT_NAME chrono-client-example-writer
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
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

set_target_properties(client_lib_reader_example PROPERTIES
    OUTPUT_NAME chrono-client-example-reader
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
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

set_target_properties(client_lib_basic_example PROPERTIES
    OUTPUT_NAME chrono-client-example-basic
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# Distributed telemetry writer example
# Note: margo.h is included directly, so we need thallium includes
add_executable(client_lib_distributed_telemetry_writer_example
    client_lib_distributed_telemetry_writer_example.cpp
)

target_include_directories(client_lib_distributed_telemetry_writer_example PRIVATE
    ${CMAKE_BINARY_DIR}/Client/cpp/include
)

target_link_libraries(client_lib_distributed_telemetry_writer_example
    PRIVATE
        chronolog_client
        thallium
        pthread
        rt
        ${MPI_CXX_LIBRARIES}
)

set_target_properties(client_lib_distributed_telemetry_writer_example PROPERTIES
    OUTPUT_NAME chrono-client-example-distributed-telemetry-writer
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# Reader-to-CSV example (replay story and write events to CSV; originally PEARC'25 demo)
add_executable(client_lib_reader_to_csv_example
    client_lib_reader_to_csv_example.cpp
)

target_include_directories(client_lib_reader_to_csv_example PRIVATE
    ${CMAKE_BINARY_DIR}/Client/cpp/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_libraries(client_lib_reader_to_csv_example
    PRIVATE
        chronolog_client
        pthread
        rt
)

set_target_properties(client_lib_reader_to_csv_example PROPERTIES
    OUTPUT_NAME chrono-client-example-reader-to-csv
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Install example binaries to chronolog/examples/
# Installed names follow chronolog install convention: chrono-<component>-example-<role>
#------------------------------------------------------------------------------
chronolog_install_target(client_lib_writer_example DESTINATION examples)
chronolog_install_target(client_lib_reader_example DESTINATION examples)
chronolog_install_target(client_lib_basic_example DESTINATION examples)
chronolog_install_target(client_lib_distributed_telemetry_writer_example DESTINATION examples)
chronolog_install_target(client_lib_reader_to_csv_example DESTINATION examples)

# Client configuration file is installed by Client/cpp to avoid duplication
