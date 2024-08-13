cmake_minimum_required(VERSION 3.19)

include_directories(../include)
include_directories(${HDF5_INCLUDE_DIRS})

# JSON-based archiver implementation
add_executable(hdf5_archiver_test
    hdf5_archiver_test.cpp
    ../include/StoryWriter.h
    ../src/StoryWriter.cpp
    ../include/StoryReader.h
    ../src/StoryReader.cpp
    story_chunk_test_utils.h
    ../../ChronoAPI/ChronoLog/src/chrono_monitor.cpp
    ../../chrono_common/StoryChunk.cpp)

target_include_directories(hdf5_archiver_test PRIVATE ../../ChronoKeeper
    ../../chrono_common
    ../../ChronoKeeper/include
    ./)
target_link_libraries(hdf5_archiver_test ${HDF5_LIBRARIES})
target_link_libraries(hdf5_archiver_test thallium)
add_test(NAME hdf5_archiver_test COMMAND hdf5_archiver_test)

# compound-vlen-string-based archiver implementation
#add_executable(cmp_vlen_str_dtype_test
#    cmp_vlen_str_dtype_test.cpp
#    story_chunk_test_utils.h
#    ../../ChronoAPI/ChronoLog/src/log.cpp)
#target_include_directories(cmp_vlen_str_dtype_test PRIVATE ../../ChronoKeeper
#    ./)
#target_link_libraries(cmp_vlen_str_dtype_test ${HDF5_LIBRARIES})
#add_test(NAME cmp_vlen_str_dtype_test COMMAND cmp_vlen_str_dtype_test)

# compound-vlen-bytes-based archiver implementation
#add_executable(cmp_vlen_bytes_dtype_test
#    cmp_vlen_bytes_dtype_test.cpp
#    story_chunk_test_utils.h
#    ../../ChronoAPI/ChronoLog/src/log.cpp)
#target_include_directories(cmp_vlen_bytes_dtype_test PRIVATE ../../ChronoKeeper
#    ./)
#target_link_libraries(cmp_vlen_bytes_dtype_test ${HDF5_LIBRARIES})
#add_test(NAME cmp_vlen_bytes_dtype_test COMMAND cmp_vlen_bytes_dtype_test)

# compound-vlen-bytes-based vs blob+map archiver implementation comparison
add_executable(cmp_vlen_bytes_vs_blob_map cmp_vlen_bytes_vs_blob_map.cpp
    ../../ChronoAPI/ChronoLog/src/chrono_monitor.cpp)
target_link_libraries(cmp_vlen_bytes_vs_blob_map ${HDF5_LIBRARIES} thallium)
add_test(NAME cmp_vlen_bytes_vs_blob_map COMMAND cmp_vlen_bytes_vs_blob_map)
