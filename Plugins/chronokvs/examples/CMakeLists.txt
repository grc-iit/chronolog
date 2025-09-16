add_executable(chronokvs_example chronokvs_example.cpp)
add_executable(chronokvs_writer_example chronokvs_writer_example.cpp)
add_executable(chronokvs_reader_example chronokvs_reader_example.cpp)

# Find required packages
find_package(spdlog REQUIRED)

# Include ChronoLog client headers for examples
foreach(target chronokvs_example chronokvs_writer_example chronokvs_reader_example)
    target_include_directories(${target}
        PRIVATE
            ${CMAKE_SOURCE_DIR}/Client/cpp/include  # Add path to ChronoLog client headers
    )
    target_link_libraries(${target} 
        PRIVATE 
            chronokvs
            spdlog::spdlog
    )
endforeach()

# Put examples into bin/ on install
install(TARGETS 
    chronokvs_example
    chronokvs_writer_example
    chronokvs_reader_example
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})