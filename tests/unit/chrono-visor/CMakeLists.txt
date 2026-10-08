cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
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
