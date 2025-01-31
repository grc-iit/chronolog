cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : chrono_player")

add_executable(chrono_player
    HDF5ArchiveReadingAgent.cpp)
target_include_directories(chrono_player PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include)

target_sources(chrono_player PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ChronoPlayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PlayerDataStore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ArchiveReadingAgent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PlaybackService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StoryChunkTransferAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryPipeline.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp)

target_link_libraries(chrono_player chronolog_client thallium)
target_link_libraries(chrono_player ${HDF5_LIBRARIES})

set_target_properties(chrono_player PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

add_executable(hdf5_archive_reader_test
    ${CMAKE_CURRENT_SOURCE_DIR}/HDF5ArchiveReadingAgentTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/HDF5ArchiveReadingAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunk.cpp)

target_include_directories(hdf5_archive_reader_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include)

target_link_directories(hdf5_archive_reader_test PRIVATE ${HDF5_LIBRARY_DIRS})

target_link_libraries(hdf5_archive_reader_test chronolog_client thallium)
target_link_libraries(hdf5_archive_reader_test ${HDF5_LIBRARIES})
# install binary
install(
    TARGETS chrono_player DESTINATION bin
)

# install configuration file
install(
    FILES ${CMAKE_SOURCE_DIR}/default_conf.json.in DESTINATION ${CHRONOLOG_INSTALL_CONF_DIR} RENAME default_conf.json
)
