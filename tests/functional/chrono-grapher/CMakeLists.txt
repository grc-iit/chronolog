cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
find_package(Thallium REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED json-c)
include(GoogleTest)

find_package(HDF5 REQUIRED COMPONENTS C CXX)

#------------------------------------------------------------------------------
# chrono_grapher standalone tests (Thallium/Margo)
#------------------------------------------------------------------------------

add_executable(chrono_grapher_extraction_chain_test
    chrono_grapher_extraction_chain_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/src/HDF5FileChunkExtractor.cpp
)

target_include_directories(chrono_grapher_extraction_chain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/include
    ${HDF5_INCLUDE_DIRS}
)

target_link_libraries(chrono_grapher_extraction_chain_test PRIVATE
    chrono_common
    chronolog_client
    thallium
    ${HDF5_LIBRARIES}
)

set_target_properties(chrono_grapher_extraction_chain_test PROPERTIES
    OUTPUT_NAME chronolog-test-grapher-extraction-chain
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Functional_ChronoGrapher_ExtractionChain COMMAND chrono_grapher_extraction_chain_test)
set_tests_properties(Functional_ChronoGrapher_ExtractionChain PROPERTIES DISABLED TRUE)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chrono_grapher_extraction_chain_test DESTINATION tests)
endif()

#------------------------------------------------------------------------------
# HDF5 destroy helpers (issue #574): exercise delete_story_files /
# delete_chronicle_files via filename-pattern filtering on a temp directory.
# Pure filesystem test — no Thallium fixture required at runtime.
#------------------------------------------------------------------------------
add_executable(chrono_grapher_destroy_files_test
    chrono_grapher_destroy_files_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/src/HDF5FileChunkExtractor.cpp
)

target_include_directories(chrono_grapher_destroy_files_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/include
    ${HDF5_INCLUDE_DIRS}
    ${JSONC_INCLUDE_DIRS}
)

target_link_directories(chrono_grapher_destroy_files_test PRIVATE
    ${JSONC_LIBRARY_DIRS}
)

target_link_libraries(chrono_grapher_destroy_files_test PRIVATE
    chrono_common
    chronolog_client
    thallium
    GTest::gtest_main
    ${HDF5_LIBRARIES}
    ${JSONC_LIBRARIES}
)

set_target_properties(chrono_grapher_destroy_files_test PROPERTIES
    OUTPUT_NAME chronolog-test-grapher-destroy-files
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Functional_ChronoGrapher_DestroyFiles COMMAND chrono_grapher_destroy_files_test)
set_tests_properties(Functional_ChronoGrapher_DestroyFiles PROPERTIES TIMEOUT 60)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chrono_grapher_destroy_files_test DESTINATION tests)
endif()

# Archive manifest recording (GTest) — every publish outcome and every deletion
# has to land in the manifest, since it is what a restarted Grapher replays.
add_executable(chrono_grapher_archive_manifest_test
    chrono_grapher_archive_manifest_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/src/HDF5FileChunkExtractor.cpp
)

target_include_directories(chrono_grapher_archive_manifest_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/include
    ${HDF5_INCLUDE_DIRS}
)

# GTest::gtest, not gtest_main: this target brings its own main() so Argobots is
# initialized before any test runs.
target_link_libraries(chrono_grapher_archive_manifest_test PRIVATE
    GTest::gtest
    chrono_common
    chronolog_client
    thallium
    ${HDF5_LIBRARIES}
)

set_target_properties(chrono_grapher_archive_manifest_test PROPERTIES
    OUTPUT_NAME chronolog-test-grapher-archive-manifest
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

gtest_discover_tests(chrono_grapher_archive_manifest_test TEST_PREFIX "Functional_ChronoGrapher_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chrono_grapher_archive_manifest_test DESTINATION tests)
endif()
