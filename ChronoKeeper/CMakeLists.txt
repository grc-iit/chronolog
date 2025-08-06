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
    ChronoKeeperInstance.cpp
    StoryPipeline.cpp
    KeeperDataStore.cpp
    CSVFileChunkExtractor.cpp
    StoryChunkExtractorRDMA.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ConfigurationManager.cpp
)

target_link_libraries(chrono_keeper chronolog_client thallium)

set_target_properties(chrono_keeper PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

# install binary
install(
    TARGETS chrono_keeper DESTINATION bin
)

# install configuration file
install(
    FILES ${CMAKE_SOURCE_DIR}/default_conf.json.in DESTINATION ${CHRONOLOG_INSTALL_CONF_DIR} RENAME default_conf.json
)