cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

#------------------------------------------------------------------------------
# Test Executables
#------------------------------------------------------------------------------

# HDF5 Archiver Test
add_executable(hdf5_archiver_test
    hdf5_archiver_test.cpp
    story_chunk_test_utils.h
    ${CMAKE_SOURCE_DIR}/ChronoStore/include/StoryWriter.h
    ${CMAKE_SOURCE_DIR}/ChronoStore/src/StoryWriter.cpp
    ${CMAKE_SOURCE_DIR}/ChronoStore/include/StoryReader.h
    ${CMAKE_SOURCE_DIR}/ChronoStore/src/StoryReader.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp
)

target_include_directories(hdf5_archiver_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ChronoStore/include
    ${CMAKE_SOURCE_DIR}/ChronoKeeper/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${HDF5_INCLUDE_DIRS}
    .
)

target_link_libraries(hdf5_archiver_test
    PRIVATE
        ${HDF5_LIBRARIES}
        thallium
)

add_test(NAME hdf5_archiver_test COMMAND hdf5_archiver_test)

# Compound VLEN Bytes vs Blob+Map Comparison Test
add_executable(cmp_vlen_bytes_vs_blob_map
    cmp_vlen_bytes_vs_blob_map.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_include_directories(cmp_vlen_bytes_vs_blob_map PRIVATE
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_libraries(cmp_vlen_bytes_vs_blob_map
    PRIVATE
        ${HDF5_LIBRARIES}
        thallium
)

add_test(NAME cmp_vlen_bytes_vs_blob_map COMMAND cmp_vlen_bytes_vs_blob_map)
