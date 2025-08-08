cmake_minimum_required(VERSION 3.14)
project(ChronoLogTests)

find_package(GTest REQUIRED)
include(GoogleTest)

# Build StoryChunkTest 
add_executable(story_chunk_test StoryChunkTest.cpp)
target_link_libraries(story_chunk_test
  PRIVATE
    GTest::gtest_main
    chronolog_client
)
gtest_discover_tests(story_chunk_test)

# Build StoryPipelineTest
set(STORYPIPE_SOURCES
  StoryPipelineTest.cpp
  ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryPipeline.cpp
)
add_executable(story_pipeline_test ${STORYPIPE_SOURCES})
target_include_directories(story_pipeline_test
  PRIVATE
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include
    ${CMAKE_SOURCE_DIR}/chrono_common
)
target_link_libraries(story_pipeline_test
  PRIVATE
    GTest::gtest_main
    chronolog_client
)
gtest_discover_tests(story_pipeline_test)