cmake_minimum_required(VERSION 3.25)

#message("ChronoEmulator CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("ChronoEmulator CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : ChronoEmulator")

# Define the project and specify the required C++ standard
project(ChronoEmulator LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source files for the library
set(SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/chronoemulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/story.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/chronicle.cpp
)

# Define the library target
add_library(ChronoEmulator STATIC ${SOURCE_FILES})

# Use target_include_directories instead of include_directories
target_include_directories(ChronoEmulator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# If there are any dependencies, link them here
# target_link_libraries(chronoemulator other_libraries)

# For installation (optional)
install(TARGETS ChronoEmulator DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)

# Testing (optional)
# enable_testing()
# add_subdirectory(tests)

# Examples (optional)
add_subdirectory(examples)

# Documentation (optional, add if you have custom CMake setup for docs)
# add_subdirectory(docs)
