add_executable(chronokvs_writer_example chronokvs_writer_example.cpp)
add_executable(chronokvs_reader_example chronokvs_reader_example.cpp)

target_link_libraries(chronokvs_writer_example PRIVATE chronokvs)
target_link_libraries(chronokvs_reader_example PRIVATE chronokvs)

# Put examples into bin/ on install
install(TARGETS 
    chronokvs_writer_example
    chronokvs_reader_example
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})