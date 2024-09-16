cmake_minimum_required(VERSION 3.25)

#message("ChronoStore Tests CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("ChronoStore Tests CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : ChronoStore Tests")

include_directories(${CMAKE_SOURCE_DIR}/ChronoStore/include)
include_directories(${HDF5_INCLUDE_DIRS})

# JSON-based archiver implementation
add_executable(hdf5_archiver_test
    ${CMAKE_CURRENT_SOURCE_DIR}/hdf5_archiver_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/story_chunk_test_utils.h
    ${CMAKE_SOURCE_DIR}/ChronoStore/include/StoryWriter.h
    ${CMAKE_SOURCE_DIR}/ChronoStore/src/StoryWriter.cpp
    ${CMAKE_SOURCE_DIR}/ChronoStore/include/StoryReader.h
    ${CMAKE_SOURCE_DIR}/ChronoStore/src/StoryReader.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunk.cpp)

target_include_directories(hdf5_archiver_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ChronoKeeper
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_SOURCE_DIR}/ChronoKeeper/include
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(hdf5_archiver_test ${HDF5_LIBRARIES})
target_link_libraries(hdf5_archiver_test thallium)
add_test(NAME hdf5_archiver_test COMMAND hdf5_archiver_test)

# compound-vlen-string-based archiver implementation
#add_executable(cmp_vlen_str_dtype_test
#    cmp_vlen_str_dtype_test.cpp
#    story_chunk_test_utils.h
#    ../../ChronoAPI/ChronoLog/src/chrono_monitor.cpp)
#target_include_directories(cmp_vlen_str_dtype_test PRIVATE ../../ChronoKeeper
#    ./)
#target_link_libraries(cmp_vlen_str_dtype_test ${HDF5_LIBRARIES})
#add_test(NAME cmp_vlen_str_dtype_test COMMAND cmp_vlen_str_dtype_test)

# compound-vlen-bytes-based archiver implementation
#add_executable(cmp_vlen_bytes_dtype_test
#    cmp_vlen_bytes_dtype_test.cpp
#    story_chunk_test_utils.h
#    ../../ChronoAPI/ChronoLog/src/chrono_monitor.cpp)
#target_include_directories(cmp_vlen_bytes_dtype_test PRIVATE ../../ChronoKeeper
#    ./)
#target_link_libraries(cmp_vlen_bytes_dtype_test ${HDF5_LIBRARIES})
#add_test(NAME cmp_vlen_bytes_dtype_test COMMAND cmp_vlen_bytes_dtype_test)

# compound-vlen-bytes-based vs blob+map archiver implementation comparison
add_executable(cmp_vlen_bytes_vs_blob_map
    ${CMAKE_CURRENT_SOURCE_DIR}/cmp_vlen_bytes_vs_blob_map.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp)
target_link_libraries(cmp_vlen_bytes_vs_blob_map ${HDF5_LIBRARIES} thallium)
add_test(NAME cmp_vlen_bytes_vs_blob_map COMMAND cmp_vlen_bytes_vs_blob_map)
