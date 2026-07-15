cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
find_package(Thallium REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# chrono_keeper unit tests
#------------------------------------------------------------------------------

# KeeperChunkRetentionStore (GTest) — header-only store, no network needed
add_executable(keeper_retention_store_test
    chrono_keeper_retention_store_test.cpp
)

target_include_directories(keeper_retention_store_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_retention_store_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(keeper_retention_store_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-retention-store)
gtest_discover_tests(keeper_retention_store_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_retention_store_test DESTINATION tests)
endif()

# Keeper extraction-chain disposal seam (GTest) — dispose_chunk routes drain
# outcomes into the retention store; shim mode (expects_watermarks()==false)
# frees on ack. Pulls DualEndpointChunkExtractorRDMA.cpp directly because it is
# only compiled into the chrono_keeper executable.
add_executable(keeper_extraction_chain_test
    chrono_keeper_extraction_chain_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/DualEndpointChunkExtractorRDMA.cpp
)

target_include_directories(keeper_extraction_chain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_extraction_chain_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(keeper_extraction_chain_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-extraction-chain)
gtest_discover_tests(keeper_extraction_chain_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_extraction_chain_test DESTINATION tests)
endif()

# Release-before-decay data-loss reproduction. KeeperDataStore/KeeperStoryPipeline
# are only compiled into the chrono_keeper executable (no shared lib), so pull the
# two .cpp files directly into the test target.
add_executable(keeper_release_dataloss_test
    chrono_keeper_release_dataloss_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/KeeperStoryPipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/KeeperDataStore.cpp
)

target_include_directories(keeper_release_dataloss_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_release_dataloss_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(keeper_release_dataloss_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-release-dataloss)
gtest_discover_tests(keeper_release_dataloss_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_release_dataloss_test DESTINATION tests)
endif()

# Orphan recovery (GTest) — IngestionQueue orphan helpers + seal-to-extraction flow
add_executable(keeper_orphan_recovery_test
    chrono_keeper_orphan_recovery_test.cpp
)

target_include_directories(keeper_orphan_recovery_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_orphan_recovery_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(keeper_orphan_recovery_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-orphan-recovery)
gtest_discover_tests(keeper_orphan_recovery_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_orphan_recovery_test DESTINATION tests)
endif()
