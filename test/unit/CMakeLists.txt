cmake_minimum_required(VERSION 3.14)
project(ChronoLogTests)

find_package(GTest REQUIRED)

add_executable(story_chunk_test StoryChunkTest.cpp)
add_executable(story_pipeline_test StoryPipelineTest.cpp)

target_link_libraries(story_chunk_test
  PRIVATE
    GTest::gtest_main
    chronolog_client
)

target_link_libraries(story_pipeline_test
  PRIVATE
    GTest::gtest_main
    chronolog_client
)

include(GoogleTest)
gtest_discover_tests(story_chunk_test)
gtest_discover_tests(story_pipeline_test)