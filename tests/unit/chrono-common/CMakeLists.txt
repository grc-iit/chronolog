cmake_minimum_required(VERSION 3.25)

find_package(HDF5 REQUIRED COMPONENTS C CXX)
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

# Conf template (GTest): the timing relationships the shipped template must keep
add_executable(conf_template_test
    chrono_common_conf_template_test.cpp
)

target_include_directories(conf_template_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_compile_definitions(conf_template_test PRIVATE
    CHRONOLOG_CONF_TEMPLATE="${CMAKE_SOURCE_DIR}/conf/default_conf.json.in"
)

target_link_libraries(conf_template_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(conf_template_test PROPERTIES OUTPUT_NAME chronolog-test-common-conf-template)
gtest_discover_tests(conf_template_test TEST_PREFIX "Unit_ChronoCommon_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(conf_template_test DESTINATION tests)
endif()

# DataStoreConf (GTest): the DataStoreInternals knobs and their range checks
add_executable(data_store_conf_test
    chrono_common_data_store_conf_test.cpp
)

target_include_directories(data_store_conf_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(data_store_conf_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(data_store_conf_test PROPERTIES OUTPUT_NAME chronolog-test-common-data-store-conf)
gtest_discover_tests(data_store_conf_test TEST_PREFIX "Unit_ChronoCommon_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(data_store_conf_test DESTINATION tests)
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

# StoryChunkWriter (GTest): how a window reaches the archive -- written through a
# temporary name and moved into place, and opened without HDF5 file locking
add_executable(story_chunk_writer_test
    chrono_common_story_chunk_writer_test.cpp
)

target_include_directories(story_chunk_writer_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${HDF5_INCLUDE_DIRS}
)

target_link_directories(story_chunk_writer_test PRIVATE ${HDF5_LIBRARY_DIRS})

target_link_libraries(story_chunk_writer_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    ${HDF5_LIBRARIES}
)

set_target_properties(story_chunk_writer_test PROPERTIES OUTPUT_NAME chronolog-test-common-story-chunk-writer)
gtest_discover_tests(story_chunk_writer_test TEST_PREFIX "Unit_ChronoCommon_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(story_chunk_writer_test DESTINATION tests)
endif()

# StoryChunkExtractorCSV (GTest): an empty window leaves no file behind
add_executable(chunk_extractor_csv_test
    chrono_common_chunk_extractor_csv_test.cpp
)

target_include_directories(chunk_extractor_csv_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
)

target_link_libraries(chunk_extractor_csv_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(chunk_extractor_csv_test PROPERTIES OUTPUT_NAME chronolog-test-common-chunk-extractor-csv)
gtest_discover_tests(chunk_extractor_csv_test TEST_PREFIX "Unit_ChronoCommon_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chunk_extractor_csv_test DESTINATION tests)
endif()
