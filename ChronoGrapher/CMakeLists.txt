cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)

#message("Chronographer CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("Chronographer CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : chrono_grapher")

add_executable(chrono_grapher)
target_include_directories(chrono_grapher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include)

target_sources(chrono_grapher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ChronoGrapher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StoryPipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/KeeperDataStore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StoryChunkExtractor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CSVFileChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp)

target_link_libraries(chrono_grapher chronolog_client thallium)

configure_file(${CMAKE_SOURCE_DIR}/default_conf.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json COPYONLY)

set_target_properties(chrono_grapher PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

# install binary
install(
    TARGETS chrono_grapher DESTINATION bin
)

# install configuration file
install(
    FILES ${CMAKE_SOURCE_DIR}/default_conf.json.in DESTINATION ${CHRONOLOG_INSTALL_DATA_DIR} RENAME default_conf.json
)
