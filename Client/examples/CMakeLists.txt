# Client/examples/CMakeLists.txt
cmake_minimum_required(VERSION 3.25)

message("Building Client/examples")

# Writer example
add_executable(client_lib_writer_example client_lib_writer_example.cpp)
target_include_directories(client_lib_writer_example PRIVATE ${CMAKE_BINARY_DIR}/Client/include)
target_link_libraries(client_lib_writer_example chronolog_client -lpthread -lrt)

# Reader example
add_executable(client_lib_reader_example client_lib_reader_example.cpp)
target_include_directories(client_lib_reader_example PRIVATE ${CMAKE_BINARY_DIR}/Client/include)
target_link_libraries(client_lib_reader_example chronolog_client -lpthread -lrt)

# Basic (read-write) example
add_executable(client_lib_basic_example client_lib_basic_example.cpp)
target_include_directories(client_lib_basic_example PRIVATE ${CMAKE_BINARY_DIR}/Client/include)
target_link_libraries(client_lib_basic_example chronolog_client -lpthread -lrt)

# Distributed Telemetry Writer example
add_executable(client_lib_distributed_telemetry_writer client_lib_distributed_telemetry_writer.cpp)
target_include_directories(client_lib_distributed_telemetry_writer PRIVATE ${CMAKE_BINARY_DIR}/Client/include)
target_link_libraries(client_lib_distributed_telemetry_writer chronolog_client -lpthread -lrt)

# Optionally: install the example binaries
install(TARGETS
    client_lib_writer_example
    client_lib_reader_example
    client_lib_basic_example
    client_lib_distributed_telemetry_writer
    DESTINATION bin
)

# Install client configuration file
install(
    FILES ${CMAKE_SOURCE_DIR}/default_client_conf.json.in
    DESTINATION ${CHRONOLOG_INSTALL_CONF_DIR}
    RENAME default_client_conf.json
)