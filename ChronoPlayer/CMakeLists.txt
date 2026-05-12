cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

#------------------------------------------------------------------------------
# ChronoPlayer Executable
#------------------------------------------------------------------------------
add_executable(chrono_player)

target_include_directories(chrono_player PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_sources(chrono_player PRIVATE
    src/ChronoPlayer.cpp
    src/ChronoPlayerConfiguration.cpp
    src/PlayerDataStore.cpp
    src/ArchiveReadingAgent.cpp
    src/HDF5ArchiveReadingAgent.cpp
    src/PlaybackService.cpp
    src/StoryChunkExtractor.cpp
    src/StoryChunkTransferAgent.cpp
)

target_link_libraries(chrono_player
    PRIVATE
        chrono_common
        chronolog_client
        thallium
        ${HDF5_LIBRARIES}
)

set_target_properties(chrono_player PROPERTIES
    OUTPUT_NAME chrono-player
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Test executables: built from test/unit/ChronoPlayer/
#------------------------------------------------------------------------------

#------------------------------------------------------------------------------
# Install executables
#------------------------------------------------------------------------------
chronolog_install_target(chrono_player)

# Configuration file is installed by ChronoVisor to avoid duplication
