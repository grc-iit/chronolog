cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

#### TransferAgentTest
add_executable(transfer_agent_test
    TransferAgentTest.cpp
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/src/StoryChunkTransferAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp)

target_include_directories(transfer_agent_test PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)
target_link_libraries(transfer_agent_test chronolog_client thallium)

set_target_properties(transfer_agent_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#### PlaybackServiceTest
add_executable(playback_service_test
    PlaybackServiceTest.cpp
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/src/PlaybackService.cpp
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/src/StoryChunkTransferAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp)

target_include_directories(playback_service_test PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)
target_link_libraries(playback_service_test chronolog_client thallium)

set_target_properties(playback_service_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#### HDF5 ArchiveReader Test
add_executable(hdf5_archive_reader_test
    HDF5ArchiveReadingAgentTest.cpp
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/src/HDF5ArchiveReadingAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunkWriter.cpp)

target_include_directories(hdf5_archive_reader_test PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/ChronoPlayer/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_directories(hdf5_archive_reader_test PRIVATE ${HDF5_LIBRARY_DIRS})

target_link_libraries(hdf5_archive_reader_test chronolog_client thallium ${HDF5_LIBRARIES})

set_target_properties(hdf5_archive_reader_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

####
chronolog_install_target(playback_service_test)
chronolog_install_target(hdf5_archive_reader_test)

