cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# chrono_common unit tests (GTest)
#------------------------------------------------------------------------------

# StoryChunk (GTest)
add_executable(story_chunk_test
    chrono_common_story_chunk_test.cpp
)

target_include_directories(story_chunk_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_libraries(story_chunk_test PRIVATE
    GTest::gtest_main
    chrono_common
)

set_target_properties(story_chunk_test PROPERTIES OUTPUT_NAME chronolog-test-common-story-chunk)
gtest_discover_tests(story_chunk_test TEST_PREFIX "Unit_ChronoCommon_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(story_chunk_test DESTINATION tests)
endif()

# StoryPipeline (GTest)
add_executable(story_pipeline_test
    chrono_common_story_pipeline_test.cpp
)

target_include_directories(story_pipeline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_compile_definitions(story_pipeline_test PRIVATE CHRONOLOG_ENABLE_GTEST_FRIENDS)

target_link_libraries(story_pipeline_test PRIVATE
    GTest::gtest_main
    chrono_common
)

set_target_properties(story_pipeline_test PROPERTIES OUTPUT_NAME chronolog-test-common-story-pipeline)
gtest_discover_tests(story_pipeline_test TEST_PREFIX "Unit_ChronoCommon_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(story_pipeline_test DESTINATION tests)
endif()

# Thallium-dependent tests (extraction_chain, chunk_consumer_service)
# are disabled pending migration to ChiMod architecture.
