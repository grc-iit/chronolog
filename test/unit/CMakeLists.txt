cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# Unit test subdirectories (chrono_common, ChronoStore, ChronoPlayer)
#------------------------------------------------------------------------------
add_subdirectory(chrono_common)
add_subdirectory(ChronoStore)
add_subdirectory(ChronoPlayer)

#------------------------------------------------------------------------------
# Unit Test Executables (GTest-based, in-tree)
#------------------------------------------------------------------------------

# StoryChunkTest
add_executable(story_chunk_test
    StoryChunkTest.cpp
)

target_include_directories(story_chunk_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_libraries(story_chunk_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(story_chunk_test PROPERTIES OUTPUT_NAME chronolog-test-common-story-chunk)
gtest_discover_tests(story_chunk_test TEST_PREFIX "Unit_ChronoCommon_")
chronolog_install_target(story_chunk_test DESTINATION tests)

# StoryPipelineTest
add_executable(story_pipeline_test
    StoryPipelineTest.cpp
)

target_include_directories(story_pipeline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_libraries(story_pipeline_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(story_pipeline_test PROPERTIES OUTPUT_NAME chronolog-test-common-story-pipeline)
gtest_discover_tests(story_pipeline_test TEST_PREFIX "Unit_ChronoCommon_")
chronolog_install_target(story_pipeline_test DESTINATION tests)

# ChronoKVSLoggerTest
add_executable(chronokvs_logger_test
    ChronoKVSLoggerTest.cpp
)

target_include_directories(chronokvs_logger_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Plugins/chronokvs/include
)

target_link_libraries(chronokvs_logger_test PRIVATE
    GTest::gtest_main
)

set_target_properties(chronokvs_logger_test PROPERTIES OUTPUT_NAME chronolog-test-chronokvs-logger)
gtest_discover_tests(chronokvs_logger_test TEST_PREFIX "Unit_ChronoKVS_")
chronolog_install_target(chronokvs_logger_test DESTINATION tests)
