cmake_minimum_required(VERSION 3.17)
find_package(HDF5 REQUIRED COMPONENTS C CXX)
find_package(JSON-C REQUIRED)

# Set the build directory to a subdirectory called "build"
set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/build)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g")
include_directories(../include)
include_directories(${HDF5_INCLUDE_DIRS})

# JSON-based archiver implementation
add_executable(hdf5_archiver_test
    hdf5_archiver_test.cpp
    ../include/StoryWriter.h
    ../src/StoryWriter.cpp
    ../include/StoryReader.h
    ../src/StoryReader.cpp
    story_chunk_test_utils.h)

target_include_directories(hdf5_archiver_test PRIVATE ../../ChronoKeeper
    ../../chrono_common
    ./)
target_link_libraries(hdf5_archiver_test ${HDF5_LIBRARIES})
target_link_libraries(hdf5_archiver_test json-c)
add_test(NAME hdf5_archiver_test COMMAND hdf5_archiver_test)

# compound-vlen-string-based archiver implementation
add_executable(cmp_vlen_str_dtype_test
    cmp_vlen_str_dtype_test.cpp
    story_chunk_test_utils.h)
target_include_directories(cmp_vlen_str_dtype_test PRIVATE ../../ChronoKeeper
    ./)
target_link_libraries(cmp_vlen_str_dtype_test ${HDF5_LIBRARIES})
add_test(NAME cmp_vlen_str_dtype_test COMMAND cmp_vlen_str_dtype_test)

# compound-vlen-bytes-based archiver implementation
add_executable(cmp_vlen_bytes_dtype_test
    cmp_vlen_bytes_dtype_test.cpp
    story_chunk_test_utils.h)
target_include_directories(cmp_vlen_bytes_dtype_test PRIVATE ../../ChronoKeeper
    ./)
target_link_libraries(cmp_vlen_bytes_dtype_test ${HDF5_LIBRARIES})
add_test(NAME cmp_vlen_bytes_dtype_test COMMAND cmp_vlen_bytes_dtype_test)

# compound-vlen-bytes-based vs blob+map archiver implementation comparison
add_executable(cmp_vlen_bytes_vs_blob_map cmp_vlen_bytes_vs_blob_map.cpp)
target_link_libraries(cmp_vlen_bytes_vs_blob_map ${HDF5_LIBRARIES} json-c)
