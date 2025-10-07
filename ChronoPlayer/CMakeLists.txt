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
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_link_libraries(chrono_player chronolog_client thallium)
target_link_libraries(chrono_player ${HDF5_LIBRARIES})

set_target_properties(chrono_player PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_subdirectory(test)

# install executables
install(
    TARGETS chrono_player DESTINATION bin
)

# install configuration file
install(
    FILES ${CMAKE_SOURCE_DIR}/default_conf.json.in DESTINATION ${CHRONOLOG_INSTALL_CONF_DIR} RENAME default_conf.json
)
