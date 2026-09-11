cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

#------------------------------------------------------------------------------
# ChronoPlayer unit tests
#------------------------------------------------------------------------------

add_executable(hdf5_archive_reader_test
    chrono_player_hdf5_archive_reader_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-player/src/ChronoPlayerConfiguration.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-player/src/HDF5ArchiveReadingAgent.cpp
)

target_include_directories(hdf5_archive_reader_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-player/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_directories(hdf5_archive_reader_test PRIVATE ${HDF5_LIBRARY_DIRS})
target_link_libraries(hdf5_archive_reader_test PRIVATE
    chrono_common
    chronolog_client
    thallium
    ${HDF5_LIBRARIES}
)

set_target_properties(hdf5_archive_reader_test PROPERTIES
    OUTPUT_NAME chronolog-test-player-hdf5-archive-reader
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Unit_ChronoPlayer_HDF5ArchiveReadingAgent COMMAND hdf5_archive_reader_test)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(hdf5_archive_reader_test DESTINATION tests)
endif()

#------------------------------------------------------------------------------

add_executable(fs_monitoring_test
    chrono_player_fs_monitoring_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-player/src/HDF5ArchiveReadingAgent.cpp
    ${CMAKE_SOURCE_DIR}/client/cpp/src/chrono_monitor.cpp
)

target_include_directories(fs_monitoring_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-player/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_directories(fs_monitoring_test PRIVATE ${HDF5_LIBRARY_DIRS})
target_link_libraries(fs_monitoring_test PRIVATE
    chrono_common
    chronolog_client
    thallium
    ${HDF5_LIBRARIES}
)

set_target_properties(fs_monitoring_test PROPERTIES
    OUTPUT_NAME chronolog-test-player-fs-monitoring
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Unit_ChronoPlayer_FileSystemMonitoring COMMAND fs_monitoring_test)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(fs_monitoring_test DESTINATION tests)
endif()

#------------------------------------------------------------------------------

# HotRangeSplit (GTest) — the replay split between archive and keepers;
# header-only, no network needed
find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(hot_range_split_test
    chrono_player_hot_range_split_test.cpp
)

target_include_directories(hot_range_split_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-player/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(hot_range_split_test PRIVATE
    GTest::gtest_main
    chronolog_client
    thallium
)

set_target_properties(hot_range_split_test PROPERTIES OUTPUT_NAME chronolog-test-player-hot-range-split)
gtest_discover_tests(hot_range_split_test TEST_PREFIX "Unit_ChronoPlayer_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(hot_range_split_test DESTINATION tests)
endif()
