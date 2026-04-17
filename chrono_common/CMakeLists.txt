cmake_minimum_required(VERSION 3.25)

# HDF5 discovery: modern HDF5 (≥ 2.x) installs a CMake package config
# to <prefix>/cmake/ (e.g. /usr/local/cmake/hdf5-config.cmake). Its
# VALID_COMPONENTS are {static, shared}, NOT the legacy {C, CXX, HL}
# that FindHDF5.cmake (module mode) uses. So
# `find_package(HDF5 COMPONENTS C CXX CONFIG)` would fail on a valid
# HDF5 2.x install, and the module fallback also fails because 2.x
# doesn't ship the h5cc wrapper layout FindHDF5 probes.
#
# Strategy: try CONFIG mode with only the "shared" component — that's
# what HDF5's Config file actually understands — and pick up the CXX
# library by name from the exported targets. Fall back to the module
# for apt/spack installs that do fit the legacy layout.
find_package(HDF5 QUIET COMPONENTS shared CONFIG)
if(NOT HDF5_FOUND)
    find_package(HDF5 REQUIRED COMPONENTS C CXX)
endif()
# CONFIG mode exports imported targets (hdf5-shared, hdf5_cpp-shared,
# HDF5::HDF5, …) instead of populating HDF5_LIBRARIES. Synthesize the
# variable so the target_link_libraries call below still works.
if(NOT HDF5_LIBRARIES)
    foreach(_hdf5_tgt hdf5_cpp-shared hdf5-shared HDF5::HDF5 hdf5::hdf5_cpp hdf5::hdf5)
        if(TARGET ${_hdf5_tgt})
            list(APPEND HDF5_LIBRARIES ${_hdf5_tgt})
        endif()
    endforeach()
endif()
if(NOT HDF5_LIBRARIES)
    message(FATAL_ERROR "chrono_common: HDF5 located but no usable \
imported target found (expected hdf5_cpp-shared, hdf5-shared, or \
HDF5::HDF5).")
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
