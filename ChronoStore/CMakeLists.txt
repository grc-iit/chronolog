find_package(HDF5 REQUIRED)
find_package(JSON-C REQUIRED)

include_directories(include
    ../ChronoAPI/ChronoLog/include
    ../ChronoKeeper)

option(BUILD_TEST_DIRECTORY "Build Test Directory" ON)
if(BUILD_TEST_DIRECTORY)
    add_subdirectory(test)
endif()

include_directories(${HDF5_INCLUDE_DIRS})
include_directories(${JSON-C_INCLUDE_DIRS})
