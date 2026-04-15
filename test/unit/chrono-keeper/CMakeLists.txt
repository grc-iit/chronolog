cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# Keeper domain logic unit tests (GTest)
# Tests the keeper's business logic patterns using chrono_common types
# without requiring the full Chimaera runtime.
#------------------------------------------------------------------------------
add_executable(keeper_runtime_test
    keeper_runtime_test.cpp
)

target_include_directories(keeper_runtime_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_libraries(keeper_runtime_test PRIVATE
    GTest::gtest_main
    chrono_common
)

set_target_properties(keeper_runtime_test PROPERTIES
    OUTPUT_NAME chronolog-test-keeper-runtime
)
gtest_discover_tests(keeper_runtime_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_runtime_test DESTINATION tests)
endif()

#------------------------------------------------------------------------------
# Grapher, Store, Player domain logic unit tests (GTest)
#------------------------------------------------------------------------------
add_executable(grapher_store_player_test
    grapher_store_player_test.cpp
)

target_include_directories(grapher_store_player_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_libraries(grapher_store_player_test PRIVATE
    GTest::gtest_main
    chrono_common
    stdc++fs
)

set_target_properties(grapher_store_player_test PROPERTIES
    OUTPUT_NAME chronolog-test-grapher-store-player
)
gtest_discover_tests(grapher_store_player_test TEST_PREFIX "Unit_ChiMod_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(grapher_store_player_test DESTINATION tests)
endif()
