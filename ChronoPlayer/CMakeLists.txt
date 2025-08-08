cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

message("build target : chrono_player")

add_executable(chrono_player  )

target_include_directories(chrono_player PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_sources(chrono_player PRIVATE
    src/ChronoPlayer.cpp
    src/PlayerDataStore.cpp
    src/ArchiveReadingAgent.cpp
    src/HDF5ArchiveReadingAgent.cpp
    src/PlaybackService.cpp
    src/StoryChunkTransferAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunkWriter.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryPipeline.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/Client/src/chrono_monitor.cpp
)

target_link_libraries(chrono_player chronolog_client thallium)
target_link_libraries(chrono_player ${HDF5_LIBRARIES})

set_target_properties(chrono_player PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

#### TransferAgentTest
add_executable(transfer_agent_test
    src/TransferAgentTest.cpp
    src/StoryChunkTransferAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp)

target_include_directories(transfer_agent_test PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)
target_link_libraries(transfer_agent_test chronolog_client thallium)

#### PlaybackServiceTest
add_executable(playback_service_test
    src/PlaybackServiceTest.cpp
    src/PlaybackService.cpp
    src/StoryChunkTransferAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp)

target_include_directories(playback_service_test PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)
target_link_libraries(playback_service_test chronolog_client thallium)

#### HDF5 ArchiveReader Test
add_executable(hdf5_archive_reader_test
    src/HDF5ArchiveReadingAgentTest.cpp
    src/HDF5ArchiveReadingAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunkWriter.cpp)

target_include_directories(hdf5_archive_reader_test PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

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
