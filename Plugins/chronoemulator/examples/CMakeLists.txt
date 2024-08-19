cmake_minimum_required(VERSION 3.25)

#message("ChronoEmulator_Examples CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("ChronoEmulator_Examples CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : ChronoEmulatorTest")

project(ChronoEmulator_Examples LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add executable for the sample app
add_executable(ChronoEmulatorTest ${CMAKE_CURRENT_SOURCE_DIR}/test_chronoEmulator.cpp)

# Link the ChronoLog Emulator library with the sample app
target_link_libraries(ChronoEmulatorTest ChronoEmulator)

# Use target_include_directories to include the ChronoLog Emulator headers
target_include_directories(ChronoEmulatorTest PRIVATE ${CMAKE_SOURCE_DIR}/Plugins/chronoemulator/include)
