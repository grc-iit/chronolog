cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

#------------------------------------------------------------------------------
# ChronoStore unit tests
#------------------------------------------------------------------------------

add_executable(hdf5_archiver_test
    hdf5_archiver_test.cpp
    story_chunk_test_utils.h
    ${CMAKE_SOURCE_DIR}/ChronoStore/include/StoryWriter.h
    ${CMAKE_SOURCE_DIR}/ChronoStore/src/StoryWriter.cpp
    ${CMAKE_SOURCE_DIR}/ChronoStore/include/StoryReader.h
    ${CMAKE_SOURCE_DIR}/ChronoStore/src/StoryReader.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_include_directories(hdf5_archiver_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ChronoStore/include
    ${CMAKE_SOURCE_DIR}/ChronoKeeper/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${HDF5_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(hdf5_archiver_test PRIVATE
    chrono_common
    ${HDF5_LIBRARIES}
    thallium
)

set_target_properties(hdf5_archiver_test PROPERTIES
    OUTPUT_NAME chronolog-test-store-hdf5-archiver
)

add_test(NAME Unit_ChronoStore_HDF5Archiver COMMAND hdf5_archiver_test)
chronolog_install_target(hdf5_archiver_test DESTINATION tests)

#------------------------------------------------------------------------------
# Compound VLEN Bytes vs Blob+Map comparison test
#------------------------------------------------------------------------------

add_executable(cmp_vlen_bytes_vs_blob_map
    cmp_vlen_bytes_vs_blob_map.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_include_directories(cmp_vlen_bytes_vs_blob_map PRIVATE
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_libraries(cmp_vlen_bytes_vs_blob_map PRIVATE
    ${HDF5_LIBRARIES}
    thallium
)

set_target_properties(cmp_vlen_bytes_vs_blob_map PROPERTIES
    OUTPUT_NAME chronolog-test-store-vlen-bytes-vs-blob-map
)

add_test(NAME Unit_ChronoStore_CmpVlenBytesVsBlobMap COMMAND cmp_vlen_bytes_vs_blob_map)
chronolog_install_target(cmp_vlen_bytes_vs_blob_map DESTINATION tests)
