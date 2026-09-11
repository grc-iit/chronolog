cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# chrono_grapher unit tests
#------------------------------------------------------------------------------

# StoryWatermarkRegistry (GTest) — header-only registry, no network needed
add_executable(story_watermark_registry_test
    story_watermark_registry_test.cpp
)

target_include_directories(story_watermark_registry_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(story_watermark_registry_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(story_watermark_registry_test PROPERTIES OUTPUT_NAME chronolog-test-story-watermark-registry)
gtest_discover_tests(story_watermark_registry_test TEST_PREFIX "Unit_ChronoGrapher_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(story_watermark_registry_test DESTINATION tests)
endif()

# HDF5FileChunkExtractor (GTest) — which chunks advance W; writes real HDF5
# files into a per-test temp directory, no network needed
find_package(Thallium REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

add_executable(hdf5_file_chunk_extractor_test
    hdf5_file_chunk_extractor_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/src/HDF5FileChunkExtractor.cpp
)

target_include_directories(hdf5_file_chunk_extractor_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${HDF5_INCLUDE_DIRS}
)

target_link_directories(hdf5_file_chunk_extractor_test PRIVATE ${HDF5_LIBRARY_DIRS})
target_link_libraries(hdf5_file_chunk_extractor_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    thallium
    ${HDF5_LIBRARIES}
)

set_target_properties(hdf5_file_chunk_extractor_test PROPERTIES OUTPUT_NAME chronolog-test-hdf5-file-chunk-extractor)
gtest_discover_tests(hdf5_file_chunk_extractor_test TEST_PREFIX "Unit_ChronoGrapher_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(hdf5_file_chunk_extractor_test DESTINATION tests)
endif()
