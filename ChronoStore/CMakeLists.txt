cmake_minimum_required(VERSION 3.17)
project(chronostore)

find_package(HDF5 REQUIRED)
find_package(JSON-C REQUIRED)

# Set the build directory to a subdirectory called "build and add compiler '-o3' flag"
set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/build)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g")

include_directories(include
        ../ChronoAPI/ChronoLog/include)

option(BUILD_TEST_DIRECTORY "Build Test Directory" ON)
if(BUILD_TEST_DIRECTORY)
    add_subdirectory(test)
endif()

include_directories(${HDF5_INCLUDE_DIRS})
include_directories(${JSON-C_INCLUDE_DIRS})

add_executable(chronostore main.cpp src/storywriter.cpp src/storyreader.cpp)
target_link_libraries(chronostore ${HDF5_LIBRARIES} json-c)