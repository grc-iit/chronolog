cmake_minimum_required(VERSION 3.25)

find_package(Threads REQUIRED)
set(CHRONOLOG_GEN_INCLUDE_DIR "${CMAKE_BINARY_DIR}/Client/include")
set(GRAFANA_STREAM_PUBLIC_INCLUDE
    ${CHRONOLOG_GEN_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/GrafanaStream/StreamingScripts
)

add_executable(client_writer
    client_writer.cpp
)

target_include_directories(client_writer
    PRIVATE
    ${GRAFANA_STREAM_PUBLIC_INCLUDE}
)

target_link_libraries(client_writer
    PRIVATE
    chronolog_client
    Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(client_writer PRIVATE rt)
endif()

target_compile_features(client_writer PRIVATE cxx_std_17)

add_executable(client_reader_stream_influx
    client_reader_stream_influx.cpp
)

target_include_directories(client_reader_stream_influx
    PRIVATE
    ${GRAFANA_STREAM_PUBLIC_INCLUDE}
)

target_link_libraries(client_reader_stream_influx
    PRIVATE
    grafana_stream
    chronolog_client
    Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(client_reader_stream_influx PRIVATE rt)
endif()

target_compile_features(client_reader_stream_influx PRIVATE cxx_std_17)

# Install executables
chronolog_install_target(client_writer)
chronolog_install_target(client_reader_stream_influx)
