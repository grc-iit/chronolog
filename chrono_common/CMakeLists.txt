cmake_minimum_required(VERSION 3.25)

find_package(HDF5 REQUIRED COMPONENTS C CXX)
find_package(spdlog REQUIRED)

#------------------------------------------------------------------------------
# chrono_common Library
#------------------------------------------------------------------------------
add_library(chrono_common STATIC
    src/city.cpp
    src/ConfigurationManager.cpp
    src/StoryChunk.cpp
    src/StoryChunkWriter.cpp
    src/StoryPipeline.cpp
    src/chrono_monitor.cpp
    src/ChronologClient.cpp
    src/CteHelper.cpp
)

target_include_directories(chrono_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(chrono_common
    PUBLIC
        chronolog_keeper_client
        chronolog_player_client
        wrp_cte_core_client
    PRIVATE
        ${HDF5_LIBRARIES}
        spdlog::spdlog
)

# Get all header files from include directory
file(GLOB CHRONO_COMMON_PUBLIC_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")

set_target_properties(chrono_common PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    PUBLIC_HEADER "${CHRONO_COMMON_PUBLIC_HEADERS}"
)
