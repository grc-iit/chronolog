find_package(HDF5 REQUIRED)

# Set the build directory to a subdirectory called "build"
set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/build)

include_directories(../include)

# include_directories(${HDF5_INCLUDE_DIRS})
link_directories(/usr/lib/x86_64-linux-gnu/hdf5/serial)
link_directories(/usr/lib/x86_64-linux-gnu/hdf5/serial/include)

add_executable(chronostore_test testreadwrite.cpp ../include/storywriter.h ../src/storywriter.cpp ../include/storyreader.h ../src/storyreader.cpp)

target_link_libraries(chronostore_test hdf5)

add_test(NAME chronostore_test COMMAND chronostore_test)