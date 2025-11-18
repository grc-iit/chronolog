cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

#------------------------------------------------------------------------------
# ChronoGrapher Executable
#------------------------------------------------------------------------------
add_executable(chrono_grapher)

target_include_directories(chrono_grapher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_sources(chrono_grapher PRIVATE
    src/ChronoGrapher.cpp
    src/GrapherDataStore.cpp
    src/CSVFileChunkExtractor.cpp
    src/HDF5FileChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunkWriter.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryPipeline.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunkExtractor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_link_libraries(chrono_grapher
    PRIVATE
        chronolog_client
        thallium
        ${HDF5_LIBRARIES}
)

set_target_properties(chrono_grapher PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Install binary
#------------------------------------------------------------------------------
chronolog_install_target(chrono_grapher)

# Configuration file is installed by ChronoVisor to avoid duplication
