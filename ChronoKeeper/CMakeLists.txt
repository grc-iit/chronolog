cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)

#------------------------------------------------------------------------------
# ChronoKeeper Executable
#------------------------------------------------------------------------------
add_executable(chrono_keeper)

target_include_directories(chrono_keeper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_sources(chrono_keeper PRIVATE
    src/ChronoKeeperInstance.cpp
    src/ChronoKeeperConfiguration.cpp
    src/StoryPipeline.cpp
    src/KeeperDataStore.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/include/StoryChunkExtractionModule.h
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ConfigurationBlocks.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ChunkExtractorCSV.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ChunkExtractorRDMA.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/RDMATransferAgent.cpp
)

target_link_libraries(chrono_keeper
    PRIVATE
        chronolog_client
        thallium
)

set_target_properties(chrono_keeper PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Install binary
#------------------------------------------------------------------------------
chronolog_install_target(chrono_keeper)

# Configuration file is installed by ChronoVisor to avoid duplication
