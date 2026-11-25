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

add_executable(visor_rpc_policy_configuration_test
    rpc_policy_configuration_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/ChronoVisorConfiguration.cpp
)
target_include_directories(visor_rpc_policy_configuration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)
target_link_libraries(visor_rpc_policy_configuration_test PRIVATE GTest::gtest_main chrono_common chronolog_client)
gtest_discover_tests(visor_rpc_policy_configuration_test TEST_PREFIX "Unit_ChronoVisor_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(visor_rpc_policy_configuration_test DESTINATION tests)
endif()

add_executable(visor_config_catalog_test
    visor_config_catalog_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/ChronoVisorConfiguration.cpp
)
target_include_directories(visor_config_catalog_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)
target_link_libraries(visor_config_catalog_test PRIVATE GTest::gtest_main chrono_common chronolog_client)
gtest_discover_tests(visor_config_catalog_test TEST_PREFIX "Unit_ChronoVisor_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(visor_config_catalog_test DESTINATION tests)
endif()

# The visor's catalog store, SQLite and in-memory (GTest) — a file in a
# temporary directory, no network needed
add_executable(visor_catalog_store_test
    visor_catalog_store_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/SqliteCatalogStore.cpp
)
target_include_directories(visor_catalog_store_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)
target_link_libraries(visor_catalog_store_test PRIVATE GTest::gtest_main chrono_common chronolog_client chronolog_sqlite3)
set_target_properties(visor_catalog_store_test PROPERTIES OUTPUT_NAME chronolog-test-visor-catalog-store)
gtest_discover_tests(visor_catalog_store_test TEST_PREFIX "Unit_ChronoVisor_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(visor_catalog_store_test DESTINATION tests)
endif()

add_executable(visor_admin_deadline_test
    admin_deadline_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/KeeperRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/VisorAdminRuntime.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/VisorClientPortal.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/ClientRegistryManager.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/ChronicleMetaDirectory.cpp)
target_include_directories(visor_admin_deadline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include)
target_link_libraries(visor_admin_deadline_test PRIVATE GTest::gtest_main chrono_rpc chrono_common chronolog_client thallium)
# Native peer providers and both actual registry/portal routes are loopback.
gtest_discover_tests(visor_admin_deadline_test TEST_PREFIX "Unit_ChronoVisor_"
    PROPERTIES RUN_SERIAL TRUE ENVIRONMENT "FI_TCP_IFACE=lo")
# The directory written through to the catalog store and rebuilt from it
# (GTest), with the same loopback peers as visor_admin_deadline_test.
add_executable(visor_catalog_directory_test
    visor_catalog_directory_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/KeeperRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/VisorAdminRuntime.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/VisorClientPortal.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/ClientRegistryManager.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/ChronicleMetaDirectory.cpp)
target_include_directories(visor_catalog_directory_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include)
target_link_libraries(visor_catalog_directory_test PRIVATE GTest::gtest_main chrono_rpc chrono_common chronolog_client thallium)
set_target_properties(visor_catalog_directory_test PROPERTIES OUTPUT_NAME chronolog-test-visor-catalog-directory)
gtest_discover_tests(visor_catalog_directory_test TEST_PREFIX "Unit_ChronoVisor_"
    PROPERTIES RUN_SERIAL TRUE ENVIRONMENT "FI_TCP_IFACE=lo")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(visor_catalog_directory_test DESTINATION tests)
endif()
# Stories the visor starts on a registering keeper, and the starts a story's
# stop goes to (GTest). Both are inline in VisorAdminRuntime.h.

add_executable(keeper_restart_stories_test
    keeper_restart_stories_test.cpp
)

target_include_directories(keeper_restart_stories_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_restart_stories_test PRIVATE
    GTest::gtest_main
    chrono_rpc
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(keeper_restart_stories_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-restart-stories)
gtest_discover_tests(keeper_restart_stories_test TEST_PREFIX "Unit_ChronoVisor_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_restart_stories_test DESTINATION tests)
endif()
