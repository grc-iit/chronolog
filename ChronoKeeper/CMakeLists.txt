cmake_minimum_required(VERSION 3.19)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)

message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")

#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -Wextra -O0 -g")

add_executable(chrono_keeper)
target_include_directories(chrono_keeper PRIVATE include
    ../chrono_common
    ../ChronoAPI/ChronoLog/include)

target_sources(chrono_keeper PRIVATE
    ChronoKeeperInstance.cpp
    StoryPipeline.cpp
    KeeperDataStore.cpp
    ../chrono_common/StoryChunk.cpp
    StoryChunkExtractor.cpp
    CSVFileChunkExtractor.cpp
#    StoryChunkExtractorRDMA.cpp
    ../ChronoAPI/ChronoLog/src/log.cpp)

target_link_libraries(chrono_keeper chronolog_client thallium)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../default_conf.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json COPYONLY)
message("build target : chrono_keeper")

set_target_properties(chrono_keeper PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

# install binary
install(
    TARGETS chrono_keeper DESTINATION bin
)

# install configuration file
install(
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/../default_conf.json.in DESTINATION conf RENAME default_conf.json
)
