# Client/examples/CMakeLists.txt
cmake_minimum_required(VERSION 3.25)

message("Building Client/examples")

add_executable(client_lib_writer_example client_lib_writer_example.cpp)

target_include_directories(client_lib_writer_example PRIVATE ${CMAKE_BINARY_DIR}/Client/include)

target_link_libraries(client_lib_writer_example chronolog_client -lpthread -lrt)

# Optionally: install the example binary
install(TARGETS client_lib_writer_example DESTINATION bin)

# install client configuration file
install(
    FILES ${CMAKE_SOURCE_DIR}/default_client_conf.json.in
    DESTINATION ${CHRONOLOG_INSTALL_CONF_DIR}
    RENAME default_client_conf.json
)