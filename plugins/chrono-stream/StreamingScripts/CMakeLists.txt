cmake_minimum_required(VERSION 3.25)

add_library(grafana_stream STATIC
    InfluxDBSink.cpp
    Transform.cpp
)

set(CHRONOLOG_GEN_INCLUDE_DIR "${CMAKE_BINARY_DIR}/client/cpp/include")
set(CHRONOLOG_SRC_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/client/cpp/include")
target_include_directories(grafana_stream
  PUBLIC
    .
    ${CHRONOLOG_GEN_INCLUDE_DIR}   
    ${CHRONOLOG_SRC_INCLUDE_DIR} 
)

find_package(CURL REQUIRED)
target_link_libraries(grafana_stream PUBLIC CURL::libcurl)
target_compile_features(grafana_stream PUBLIC cxx_std_17)

# Set PUBLIC_HEADER property for automatic installation
file(GLOB HEADER_FILES "*.h")
set_target_properties(grafana_stream PROPERTIES
    PUBLIC_HEADER "${HEADER_FILES}"
)

# Install library and headers
chronolog_install_target(grafana_stream)
