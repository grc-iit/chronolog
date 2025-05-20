cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

message("build target : chrono_player")

add_executable(chrono_player  )

target_include_directories(chrono_player PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include)

target_sources(chrono_player PRIVATE
    ChronoPlayer.cpp
    PlayerDataStore.cpp
    ArchiveReadingAgent.cpp
    HDF5ArchiveReadingAgent.cpp
    PlaybackService.cpp
    StoryChunkTransferAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryPipeline.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp)

target_link_libraries(chrono_player chronolog_client thallium)
target_link_libraries(chrono_player ${HDF5_LIBRARIES})

set_target_properties(chrono_player PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

#### TransferAgentTest
add_executable(transfer_agent_test
    TransferAgentTest.cpp
    StoryChunkTransferAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunk.cpp)

target_include_directories(transfer_agent_test PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include)
target_link_libraries(transfer_agent_test chronolog_client thallium)

#### PlaybackServiceTest
add_executable(playback_service_test
    PlaybackServiceTest.cpp
    PlaybackService.cpp
    StoryChunkTransferAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunk.cpp)

target_include_directories(playback_service_test PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include)
target_link_libraries(playback_service_test chronolog_client thallium)

#### HDF5 ArchiveReader Test
add_executable(hdf5_archive_reader_test
    HDF5ArchiveReadingAgentTest.cpp
    HDF5ArchiveReadingAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunk.cpp)

target_include_directories(hdf5_archive_reader_test PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include)

target_link_directories(hdf5_archive_reader_test PRIVATE ${HDF5_LIBRARY_DIRS})

target_link_libraries(hdf5_archive_reader_test chronolog_client thallium)
target_link_libraries(hdf5_archive_reader_test ${HDF5_LIBRARIES})

####

# install executables
install(
    TARGETS chrono_player DESTINATION bin
)

# install configuration file
install(
    FILES ${CMAKE_SOURCE_DIR}/default_conf.json.in DESTINATION ${CHRONOLOG_INSTALL_CONF_DIR} RENAME default_conf.json
)
