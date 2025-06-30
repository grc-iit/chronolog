# Client/examples/CMakeLists.txt
cmake_minimum_required(VERSION 3.25)

find_package(MPI REQUIRED)

message("Building Client/examples")

add_executable(client_lib_writer_example client_lib_writer_example.cpp)
target_include_directories(client_lib_writer_example PRIVATE ${CMAKE_BINARY_DIR}/Client/include)
target_link_libraries(client_lib_writer_example chronolog_client -lpthread -lrt)
install(TARGETS client_lib_writer_example DESTINATION bin)

add_executable(client_lib_distributed_telemetry_writer client_lib_distributed_telemetry_writer.cpp)
target_include_directories(client_lib_distributed_telemetry_writer PRIVATE ${CMAKE_BINARY_DIR}/Client/include)
target_link_libraries(client_lib_distributed_telemetry_writer chronolog_client -lpthread -lrt ${MPI_CXX_LIBRARIES})
install(TARGETS client_lib_distributed_telemetry_writer DESTINATION bin)

add_executable(client_lib_telemetry_reader client_lib_telemetry_reader.cpp)
target_include_directories(client_lib_telemetry_reader PRIVATE ${CMAKE_BINARY_DIR}/Client/include)
target_link_libraries(client_lib_telemetry_reader chronolog_client -lpthread -lrt)
install(TARGETS client_lib_telemetry_reader DESTINATION bin)

# install client configuration file
install(
    FILES ${CMAKE_SOURCE_DIR}/default_client_conf.json.in
    DESTINATION ${CHRONOLOG_INSTALL_CONF_DIR}
    RENAME default_client_conf.json
)