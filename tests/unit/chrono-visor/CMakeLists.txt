cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
find_package(Thallium REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# chrono_visor unit tests
#------------------------------------------------------------------------------

# Chronicle story ids (GTest) — header-only Chronicle, no network needed
add_executable(chronicle_story_id_test
    chronicle_story_id_test.cpp
)

target_include_directories(chronicle_story_id_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(chronicle_story_id_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(chronicle_story_id_test PROPERTIES OUTPUT_NAME chronolog-test-chronicle-story-id)
gtest_discover_tests(chronicle_story_id_test TEST_PREFIX "Unit_ChronoVisor_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronicle_story_id_test DESTINATION tests)
endif()

# Recording group chosen for a story (GTest) — header-only, no network needed
add_executable(story_group_selection_test
    story_group_selection_test.cpp
)

target_include_directories(story_group_selection_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(story_group_selection_test PRIVATE
    GTest::gtest_main
)

set_target_properties(story_group_selection_test PROPERTIES OUTPUT_NAME chronolog-test-story-group-selection)
gtest_discover_tests(story_group_selection_test TEST_PREFIX "Unit_ChronoVisor_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(story_group_selection_test DESTINATION tests)
endif()

# Stories the visor starts on a registering keeper (GTest). The selection is a
# pure function, but it lives in KeeperRegistry.cpp, which needs thallium.

add_executable(keeper_restart_stories_test
    keeper_restart_stories_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/KeeperRegistry.cpp
)

target_include_directories(keeper_restart_stories_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_restart_stories_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(keeper_restart_stories_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-restart-stories)
gtest_discover_tests(keeper_restart_stories_test TEST_PREFIX "Unit_ChronoVisor_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_restart_stories_test DESTINATION tests)
endif()
