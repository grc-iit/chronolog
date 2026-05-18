cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)

#------------------------------------------------------------------------------
# Story Chunk Drain Test Executables
#------------------------------------------------------------------------------

# Extract Test
add_executable(extract_test)

target_sources(extract_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/src/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-common/src/ConfigurationBlocks.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/ChronoKeeperConfiguration.cpp
    keeper_grapher_extract_test.cpp
    ${CMAKE_SOURCE_DIR}/client/cpp/src/chrono_monitor.cpp
)

target_include_directories(extract_test PRIVATE
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
)

target_link_libraries(extract_test
    PRIVATE
        chrono_common
        thallium
)

add_test(NAME Integration_KeeperGrapher_StoryChunkExtract COMMAND extract_test)

# Ingest Test
add_executable(ingest_test)

target_sources(ingest_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/src/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-common/src/ConfigurationBlocks.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/src/ChronoGrapherConfiguration.cpp
    keeper_grapher_ingest_test.cpp
    ${CMAKE_SOURCE_DIR}/client/cpp/src/chrono_monitor.cpp
)

target_include_directories(ingest_test PRIVATE
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/include
)

target_link_libraries(ingest_test
    PRIVATE
        chrono_common
        thallium
)

add_test(NAME Integration_KeeperGrapher_StoryChunkIngest COMMAND ingest_test)

#------------------------------------------------------------------------------
# Custom Target and Configuration
#------------------------------------------------------------------------------
add_custom_target(story_chunk_drain_test)
add_dependencies(story_chunk_drain_test
    extract_test
    ingest_test
)

configure_file(
    ${CMAKE_SOURCE_DIR}/conf/default_conf.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json
    COPYONLY
)
