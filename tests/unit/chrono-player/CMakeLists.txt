cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
find_package(Thallium REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)
include(GoogleTest)

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

# Archive index construction (GTest) — manifest-backed index vs the scan fallback,
# and the window-end bound that lets a read skip a file without opening it.
add_executable(archive_index_test
    chrono_player_archive_index_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-player/src/HDF5ArchiveReadingAgent.cpp
)

target_include_directories(archive_index_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-player/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${HDF5_INCLUDE_DIRS}
)

target_link_directories(archive_index_test PRIVATE ${HDF5_LIBRARY_DIRS})
# GTest::gtest, not gtest_main: this target brings its own main() so Argobots is
# initialized before any test runs.
target_link_libraries(archive_index_test PRIVATE
    GTest::gtest
    chrono_common
    chronolog_client
    thallium
    ${HDF5_LIBRARIES}
)

set_target_properties(archive_index_test PROPERTIES
    OUTPUT_NAME chronolog-test-player-archive-index
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

gtest_discover_tests(archive_index_test TEST_PREFIX "Unit_ChronoPlayer_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(archive_index_test DESTINATION tests)
endif()
