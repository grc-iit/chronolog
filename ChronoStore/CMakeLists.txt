cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)
find_package(spdlog REQUIRED)
# Note: JSON-C dependency will be handled at the target level

#message("Chronostore CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("Chronostore CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : chrono_store")

# Note: ChronoStore currently only contains test targets and library sources
# The main executable target is not defined here, only in test subdirectory

option(BUILD_TEST_DIRECTORY "Build Test Directory" ON)
if(BUILD_TEST_DIRECTORY)
    add_subdirectory(test)
endif()
