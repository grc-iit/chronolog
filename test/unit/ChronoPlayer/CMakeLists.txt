cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

#------------------------------------------------------------------------------
# ChronoPlayer unit tests
#------------------------------------------------------------------------------

add_executable(transfer_agent_test
    TransferAgentTest.cpp
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/src/StoryChunkTransferAgent.cpp
)

target_include_directories(transfer_agent_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_libraries(transfer_agent_test PRIVATE
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(transfer_agent_test PROPERTIES
    OUTPUT_NAME chronolog-test-player-transfer-agent
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Unit_ChronoPlayer_StoryChunkTransferAgent COMMAND transfer_agent_test)
set_tests_properties(Unit_ChronoPlayer_StoryChunkTransferAgent PROPERTIES DISABLED TRUE)
chronolog_install_target(transfer_agent_test DESTINATION tests)

#------------------------------------------------------------------------------

add_executable(playback_service_test
    PlaybackServiceTest.cpp
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/src/PlaybackService.cpp
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/src/StoryChunkTransferAgent.cpp
)

target_include_directories(playback_service_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_libraries(playback_service_test PRIVATE
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(playback_service_test PROPERTIES
    OUTPUT_NAME chronolog-test-player-playback-service
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Unit_ChronoPlayer_PlaybackServiceLifecycle COMMAND playback_service_test)
set_tests_properties(Unit_ChronoPlayer_PlaybackServiceLifecycle PROPERTIES DISABLED TRUE)
chronolog_install_target(playback_service_test DESTINATION tests)

#------------------------------------------------------------------------------

add_executable(hdf5_archive_reader_test
    HDF5ArchiveReadingAgentTest.cpp
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/src/HDF5ArchiveReadingAgent.cpp
)

target_include_directories(hdf5_archive_reader_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
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
chronolog_install_target(hdf5_archive_reader_test DESTINATION tests)

#------------------------------------------------------------------------------

add_executable(fs_monitoring_test
    FSMonitoringTest.cpp
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/src/HDF5ArchiveReadingAgent.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_include_directories(fs_monitoring_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
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
chronolog_install_target(fs_monitoring_test DESTINATION tests)
