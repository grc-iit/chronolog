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
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(story_chunk_test DESTINATION tests)
endif()

# StoryPipeline (GTest)
add_executable(story_pipeline_test
    chrono_common_story_pipeline_test.cpp
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
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(story_pipeline_test DESTINATION tests)
endif()

# StoryChunk (GTest)
add_executable(extraction_chain_config_test
    chrono_common_extraction_chain_config_test.cpp
)

target_include_directories(extraction_chain_config_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_libraries(extraction_chain_config_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(extraction_chain_config_test PROPERTIES OUTPUT_NAME chronolog-test-common-extraction-chain-config)
gtest_discover_tests(extraction_chain_config_test TEST_PREFIX "Unit_ChronoCommon_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(extraction_chain_config_test DESTINATION tests)
endif()

#------------------------------------------------------------------------------
# chrono_common standalone tests (Thallium/Margo)
#------------------------------------------------------------------------------

add_executable(extraction_module_test
    chrono_common_extraction_module_test.cpp
)

target_include_directories(extraction_module_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_libraries(extraction_module_test PRIVATE
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(extraction_module_test PROPERTIES
    OUTPUT_NAME chronolog-test-common-extraction-module
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Unit_ChronoCommon_ExtractionModule COMMAND extraction_module_test)
set_tests_properties(Unit_ChronoCommon_ExtractionModule PROPERTIES DISABLED TRUE)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(extraction_module_test DESTINATION tests)
endif()

#------------------------------------------------------------------------------

add_executable(chunk_consumer_service_test
    chrono_common_chunk_consumer_service_test.cpp
)

target_include_directories(chunk_consumer_service_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_libraries(chunk_consumer_service_test PRIVATE
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(chunk_consumer_service_test PROPERTIES
    OUTPUT_NAME chronolog-test-common-chunk-consumer-service
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Unit_ChronoCommon_ChunkConsumerService COMMAND chunk_consumer_service_test)
set_tests_properties(Unit_ChronoCommon_ChunkConsumerService PROPERTIES DISABLED TRUE)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chunk_consumer_service_test DESTINATION tests)
endif()
