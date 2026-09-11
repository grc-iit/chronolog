cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
find_package(Thallium REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# chrono_common unit tests (GTest and standalone executables)
#------------------------------------------------------------------------------

# StoryChunk (GTest)
add_executable(story_chunk_test
    chrono_common_story_chunk_test.cpp
)

target_include_directories(story_chunk_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(story_chunk_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
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
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(story_pipeline_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(story_pipeline_test PROPERTIES OUTPUT_NAME chronolog-test-common-story-pipeline)
# mergeEvents regressions show up as hangs; fail them fast instead of at the
# ctest default of 1500 s
gtest_discover_tests(story_pipeline_test TEST_PREFIX "Unit_ChronoCommon_" PROPERTIES TIMEOUT 60)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(story_pipeline_test DESTINATION tests)
endif()

# ExtractionChainConfiguration (standalone, custom main -- NOT a GTest binary)
add_executable(extraction_chain_config_test
    chrono_common_extraction_chain_config_test.cpp
)

target_include_directories(extraction_chain_config_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(extraction_chain_config_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(extraction_chain_config_test PROPERTIES OUTPUT_NAME chronolog-test-common-extraction-chain-config)

add_test(NAME Unit_ChronoCommon_ExtractionChainConfig COMMAND extraction_chain_config_test)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(extraction_chain_config_test DESTINATION tests)
endif()

#------------------------------------------------------------------------------
# StoryChunkExtractionModule (GTest) — the drain loop's contract with its
# chain; Argobots only, no network needed
#------------------------------------------------------------------------------

add_executable(extraction_module_test
    chrono_common_extraction_module_test.cpp
)

target_include_directories(extraction_module_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(extraction_module_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(extraction_module_test PROPERTIES OUTPUT_NAME chronolog-test-common-extraction-module)
# a lost chunk or a missed wakeup in the drain loop shows up as a hang
gtest_discover_tests(extraction_module_test TEST_PREFIX "Unit_ChronoCommon_" PROPERTIES TIMEOUT 60)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(extraction_module_test DESTINATION tests)
endif()

