cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

#------------------------------------------------------------------------------
# ChronoPlayer Executable
#------------------------------------------------------------------------------
add_executable(chrono_player)

target_include_directories(chrono_player PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_sources(chrono_player PRIVATE
    src/ChronoPlayer.cpp
    src/PlayerDataStore.cpp
    src/ArchiveReadingAgent.cpp
    src/HDF5ArchiveReadingAgent.cpp
    src/PlaybackService.cpp
    src/StoryChunkTransferAgent.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_link_libraries(chrono_player
    PRIVATE
        chrono_common
        chronolog_client
        thallium
        ${HDF5_LIBRARIES}
)

set_target_properties(chrono_player PROPERTIES
    OUTPUT_NAME chrono-player
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Test Executables
#------------------------------------------------------------------------------

# TransferAgentTest
add_executable(transfer_agent_test
    src/TransferAgentTest.cpp
    src/StoryChunkTransferAgent.cpp
)

target_include_directories(transfer_agent_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_libraries(transfer_agent_test
    PRIVATE
        chrono_common
        chronolog_client
        thallium
)

set_target_properties(transfer_agent_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# PlaybackServiceTest
add_executable(playback_service_test
    src/PlaybackServiceTest.cpp
    src/PlaybackService.cpp
    src/StoryChunkTransferAgent.cpp
)

target_include_directories(playback_service_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_libraries(playback_service_test
    PRIVATE
        chrono_common
        chronolog_client
        thallium
)

set_target_properties(playback_service_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# HDF5 ArchiveReader Test
add_executable(hdf5_archive_reader_test
    src/HDF5ArchiveReadingAgentTest.cpp
    src/HDF5ArchiveReadingAgent.cpp
)

target_include_directories(hdf5_archive_reader_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_directories(hdf5_archive_reader_test PRIVATE
    ${HDF5_LIBRARY_DIRS}
)

target_link_libraries(hdf5_archive_reader_test
    PRIVATE
        chrono_common
        chronolog_client
        thallium
        ${HDF5_LIBRARIES}
)

set_target_properties(hdf5_archive_reader_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# File System Monitoring Test
add_executable(fs_monitoring_test
    src/FSMonitoringTest.cpp
    src/HDF5ArchiveReadingAgent.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_include_directories(fs_monitoring_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_directories(fs_monitoring_test PRIVATE
    ${HDF5_LIBRARY_DIRS}
)

target_link_libraries(fs_monitoring_test
    PRIVATE
        chrono_common
        chronolog_client
        thallium
        ${HDF5_LIBRARIES}
)

set_target_properties(fs_monitoring_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Install executables
#------------------------------------------------------------------------------
chronolog_install_target(chrono_player)

# Configuration file is installed by ChronoVisor to avoid duplication
