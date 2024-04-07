cmake_minimum_required(VERSION 3.19)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)

message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : chrono_grapher")

add_executable(chrono_grapher)
target_include_directories(chrono_grapher PRIVATE include
    ../chrono_common
    ../ChronoAPI/ChronoLog/include)

target_sources(chrono_grapher PRIVATE
    ChronoGrapher.cpp
    StoryPipeline.cpp
    KeeperDataStore.cpp
    ../chrono_common/StoryChunk.cpp
    StoryChunkExtractor.cpp
    CSVFileChunkExtractor.cpp
    ../ChronoAPI/ChronoLog/src/log.cpp)
target_link_libraries(chrono_grapher chronolog_client thallium)

#configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../default_conf.json.in
#    ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json COPYONLY)

set_target_properties(chrono_grapher PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

# install binary
install(
    TARGETS chrono_grapher DESTINATION bin
)

# install configuration file
install(
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/../default_conf.json.in DESTINATION conf RENAME default_conf.json
)
