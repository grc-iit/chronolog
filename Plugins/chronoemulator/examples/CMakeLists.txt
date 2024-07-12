cmake_minimum_required(VERSION 3.25)
project(ChronoEmulator_Examples LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add executable for the sample app
add_executable(ChronoEmulatorTest test_chronoEmulator.cpp)

# Link the ChronoLog Emulator library with the sample app
target_link_libraries(ChronoEmulatorTest ChronoEmulator)

# Use target_include_directories to include the ChronoLog Emulator headers
target_include_directories(ChronoEmulatorTest PRIVATE ${PROJECT_SOURCE_DIR}/../include)
