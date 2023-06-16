cmake_minimum_required(VERSION 3.17)
find_package(HDF5 REQUIRED)

# Set the build directory to a subdirectory called "build"
set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/build)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g")
include_directories(../include)
include_directories(${HDF5_INCLUDE_DIRS})

add_executable(chronostore_test testreadwrite.cpp ../include/storywriter.h ../src/storywriter.cpp ../include/storyreader.h ../src/storyreader.cpp)

target_link_libraries(chronostore_test ${HDF5_LIBRARIES})
add_test(NAME chronostore_test COMMAND chronostore_test)

add_executable(hdf5_archiver_test
        hdf5_archiver_test.cpp
        ../include/storywriter.h
        ../src/storywriter.cpp
        ../include/storyreader.h
        ../src/storyreader.cpp)

target_link_libraries(hdf5_archiver_test ${HDF5_LIBRARIES})
target_link_libraries(hdf5_archiver_test json-c)
add_test(NAME hdf5_archiver_test COMMAND hdf5_archiver_test)