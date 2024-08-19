cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)

#message("StoryChunkDrainTest CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("StoryChunkDrainTest CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Build Target: StoryChunkDrainTest")

add_custom_target(story_chunk_drain_test)
add_dependencies(story_chunk_drain_test extract_test ingest_test)

add_executable(extract_test)
target_sources(extract_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extract_test.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/chronolog_types.h
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp)

target_include_directories(extract_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include)

target_link_libraries(extract_test thallium)
message("build target : extract_test")

add_executable(ingest_test)
target_sources(ingest_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ingest_test.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/chronolog_types.h
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp)
target_include_directories(ingest_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include)
target_link_libraries(ingest_test thallium)
message("build target : ingest_test")

configure_file(${CMAKE_SOURCE_DIR}/default_conf.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json COPYONLY)