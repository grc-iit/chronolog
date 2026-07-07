cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
find_package(Thallium REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# chrono_keeper unit tests
#------------------------------------------------------------------------------

# KeeperTailStore (GTest) — header-only store, no network needed
add_executable(keeper_tail_store_test
    chrono_keeper_tail_store_test.cpp
)

target_include_directories(keeper_tail_store_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_tail_store_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(keeper_tail_store_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-tail-store)
gtest_discover_tests(keeper_tail_store_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_tail_store_test DESTINATION tests)
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
