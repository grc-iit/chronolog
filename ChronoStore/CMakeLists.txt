cmake_minimum_required(VERSION 3.5)
project(chronostore)

find_package(HDF5 REQUIRED)

# Set the build directory to a subdirectory called "build"
set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/build)

include_directories(include)
include_directories(test)

include_directories(${HDF5_INCLUDE_DIRS})
link_directories(/usr/lib/x86_64-linux-gnu/hdf5/serial)
link_directories(/usr/lib/x86_64-linux-gnu/hdf5/serial/include)

add_executable(chronostore src/storywriter.cpp src/storyreader.cpp test/testreadwrite.cpp)

target_link_libraries(chronostore hdf5 ${HDF5_LIBRARIES})
