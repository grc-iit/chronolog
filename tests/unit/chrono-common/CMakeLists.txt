cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)
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
gtest_discover_tests(story_pipeline_test TEST_PREFIX "Unit_ChronoCommon_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(story_pipeline_test DESTINATION tests)
endif()

# StoryChunk (GTest)
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
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
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


# ArchiveManifest (GTest) — durable record of published archive files and the
# source of restored per-story watermarks. Pure file/JSON logic, no daemons.
add_executable(archive_manifest_test
    archive_manifest_test.cpp
)

target_include_directories(archive_manifest_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(archive_manifest_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(archive_manifest_test PROPERTIES OUTPUT_NAME chronolog-test-common-archive-manifest)
gtest_discover_tests(archive_manifest_test TEST_PREFIX "Unit_ChronoCommon_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(archive_manifest_test DESTINATION tests)
endif()

# StoryChunkWriter publish semantics (GTest) — atomic publish and non-clobbering
# rotation. Writes real HDF5 files into a temp directory.
add_executable(story_chunk_writer_test
    story_chunk_writer_test.cpp
)

target_include_directories(story_chunk_writer_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
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
