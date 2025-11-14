cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)

#message("Chronokeeper CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("Chronokeeper CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : chrono_keeper")

#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -Wextra -O0 -g")

add_executable(chrono_keeper)
target_include_directories(chrono_keeper PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_sources(chrono_keeper PRIVATE
    src/ChronoKeeperInstance.cpp
    src/StoryPipeline.cpp
    src/KeeperDataStore.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/include/StoryChunkExtractionModule.h
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ChunkExtractorCSV.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ChunkExtractorRDMA.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/RDMATransferAgent.cpp
)

target_link_libraries(chrono_keeper chronolog_client thallium)

set_target_properties(chrono_keeper PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# install binary
chronolog_install_target(chrono_keeper)

# Configuration file is installed by ChronoVisor to avoid duplication
