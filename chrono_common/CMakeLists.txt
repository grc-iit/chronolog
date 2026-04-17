cmake_minimum_required(VERSION 3.25)

# HDF5 ≥ 2.x installs a CMake package config under
# <prefix>/share/cmake/hdf5; FindHDF5.cmake (module mode) doesn't know
# about that layout and reports "Could NOT find HDF5 (missing:
# HDF5_LIBRARIES HDF5_INCLUDE_DIRS C CXX)". Prefer CONFIG mode first so
# modern HDF5 installs are picked up, and fall back to the classic
# FindHDF5 module for system (apt/spack) installs.
find_package(HDF5 QUIET COMPONENTS C CXX CONFIG)
if(NOT HDF5_FOUND)
    find_package(HDF5 REQUIRED COMPONENTS C CXX)
endif()
# CONFIG mode may leave HDF5_LIBRARIES empty because it exports imported
# targets (hdf5::hdf5, hdf5::hdf5_cpp, HDF5::HDF5, …) rather than
# populating the legacy variables. Synthesize HDF5_LIBRARIES so the
# target_link_libraries(chrono_common … ${HDF5_LIBRARIES}) below still
# works under either discovery path.
if(NOT HDF5_LIBRARIES)
    foreach(_hdf5_tgt HDF5::HDF5 hdf5::hdf5_cpp hdf5::hdf5 hdf5-cpp-shared hdf5-shared)
        if(TARGET ${_hdf5_tgt})
            list(APPEND HDF5_LIBRARIES ${_hdf5_tgt})
        endif()
    endforeach()
endif()
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
