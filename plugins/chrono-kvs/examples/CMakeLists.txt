# ChronoKVS runnable examples (installed as chrono-chronokvs-example-<role>)
add_executable(chronokvs_writer_example chronokvs_writer_example.cpp)
add_executable(chronokvs_reader_example chronokvs_reader_example.cpp)

target_link_libraries(chronokvs_writer_example PRIVATE chronokvs)
target_link_libraries(chronokvs_reader_example PRIVATE chronokvs)

# cmd_arg_parse.h lives in chronolog_client's public include dir.
# chronokvs consumes chronolog_client privately, so pull its include
# interface in directly here for the examples that use the helper.
target_include_directories(chronokvs_writer_example PRIVATE
    $<TARGET_PROPERTY:chronolog_client,INTERFACE_INCLUDE_DIRECTORIES>
)
target_include_directories(chronokvs_reader_example PRIVATE
    $<TARGET_PROPERTY:chronolog_client,INTERFACE_INCLUDE_DIRECTORIES>
)

set_target_properties(chronokvs_writer_example PROPERTIES
    OUTPUT_NAME chrono-chronokvs-example-writer
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)
set_target_properties(chronokvs_reader_example PROPERTIES
    OUTPUT_NAME chrono-chronokvs-example-reader
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# Install to chronolog/examples/
chronolog_install_target(chronokvs_writer_example DESTINATION examples)
chronolog_install_target(chronokvs_reader_example DESTINATION examples)