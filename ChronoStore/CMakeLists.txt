cmake_minimum_required(VERSION 3.17)
project(chronostore)

find_package(HDF5 REQUIRED)

# Set the build directory to a subdirectory called "build"
set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/build)

include_directories(include)
include_directories(${HDF5_INCLUDE_DIRS})

add_executable(chronostore main.cpp src/storywriter.cpp src/storyreader.cpp)
target_link_libraries(chronostore ${HDF5_LIBRARIES})