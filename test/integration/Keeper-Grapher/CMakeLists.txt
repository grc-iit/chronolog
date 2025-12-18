cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)

#------------------------------------------------------------------------------
# Story Chunk Drain Test Executables
#------------------------------------------------------------------------------

# Extract Test
add_executable(extract_test)

target_sources(extract_test PRIVATE
    extract_test.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/ChronoKeeper/src/ChronoKeeperConfiguration.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_include_directories(extract_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/ChronoKeeper/include
)

target_link_libraries(extract_test
    PRIVATE
        thallium
)

# Ingest Test
add_executable(ingest_test)

target_sources(ingest_test PRIVATE
    ingest_test.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/ChronoGrapher/src/ChronoGrapherConfiguration.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_include_directories(ingest_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/ChronoGrapher/include
)

target_link_libraries(ingest_test
    PRIVATE
        thallium
)

#------------------------------------------------------------------------------
# Custom Target and Configuration
#------------------------------------------------------------------------------
add_custom_target(story_chunk_drain_test)
add_dependencies(story_chunk_drain_test
    extract_test
    ingest_test
)

configure_file(
    ${CMAKE_SOURCE_DIR}/default_conf.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json
    COPYONLY
)
