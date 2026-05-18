# ChronoSQL runnable examples (installed as chrono-chronosql-example-<role>)
add_executable(chronosql_writer_example chronosql_writer_example.cpp)
add_executable(chronosql_reader_example chronosql_reader_example.cpp)

target_link_libraries(chronosql_writer_example PRIVATE chronosql)
target_link_libraries(chronosql_reader_example PRIVATE chronosql)

# cmd_arg_parse.h lives in chronolog_client's public include dir. chronosql
# consumes chronolog_client privately, so pull its include interface in
# directly here for the examples that use the helper.
target_include_directories(chronosql_writer_example PRIVATE
    $<TARGET_PROPERTY:chronolog_client,INTERFACE_INCLUDE_DIRECTORIES>
)
target_include_directories(chronosql_reader_example PRIVATE
    $<TARGET_PROPERTY:chronolog_client,INTERFACE_INCLUDE_DIRECTORIES>
)

set_target_properties(chronosql_writer_example PROPERTIES
    OUTPUT_NAME chrono-chronosql-example-writer
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)
set_target_properties(chronosql_reader_example PROPERTIES
    OUTPUT_NAME chrono-chronosql-example-reader
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

chronolog_install_target(chronosql_writer_example DESTINATION examples)
chronolog_install_target(chronosql_reader_example DESTINATION examples)
