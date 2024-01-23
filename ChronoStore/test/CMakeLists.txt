cmake_minimum_required(VERSION 3.17)
find_package(HDF5 REQUIRED COMPONENTS C CXX)
find_package(JSON-C REQUIRED)

# Set the build directory to a subdirectory called "build"
set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/build)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g")
include_directories(../include)
include_directories(${HDF5_INCLUDE_DIRS})

add_executable(chronostore_test testreadwrite.cpp ../include/StoryWriter.h ../src/StoryWriter.cpp ../include/StoryReader.h ../src/StoryReader.cpp)

target_link_libraries(chronostore_test ${HDF5_LIBRARIES})
add_test(NAME chronostore_test COMMAND chronostore_test)

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

add_executable(cmp_vlen_str_dtype_test
    cmp_vlen_str_dtype_test.cpp
    story_chunk_test_utils.h)
target_include_directories(cmp_vlen_str_dtype_test PRIVATE ../../ChronoKeeper
    ./)
target_link_libraries(cmp_vlen_str_dtype_test ${HDF5_LIBRARIES})
add_test(NAME cmp_vlen_str_dtype_test COMMAND cmp_vlen_str_dtype_test)

add_executable(cmp_vlen_bytes_dtype_test
    cmp_vlen_bytes_dtype_test.cpp
    story_chunk_test_utils.h)
target_include_directories(cmp_vlen_bytes_dtype_test PRIVATE ../../ChronoKeeper
    ./)
target_link_libraries(cmp_vlen_bytes_dtype_test ${HDF5_LIBRARIES})
add_test(NAME cmp_vlen_bytes_dtype_test COMMAND cmp_vlen_bytes_dtype_test)

find_package(JSON-C REQUIRED)
add_executable(cmp_vlen_bytes_cpp cmp_vlen_bytes_cpp.cpp)
target_link_libraries(cmp_vlen_bytes_cpp ${HDF5_LIBRARIES} json-c)
