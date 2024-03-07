cmake_minimum_required(VERSION 3.17)

# Define the project and specify the required C++ standard
project(ChronoKVS LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(../ChronoEmulator ${PROJECT_SOURCE_DIR}/../ChronoEmulator)

# Source files for the library
set(SOURCE_FILES
    src/chronokvs.cpp
    src/chronokvs_mapper.cpp
    src/chronokvs_memorymanager.cpp
    src/chronokvs_utils.cpp
    src/chronolog_client.cpp
)

# Define the library target
add_library(ChronoKVS STATIC ${SOURCE_FILES})

# Use target_include_directories instead of include_directories
target_include_directories(ChronoKVS PUBLIC ${PROJECT_SOURCE_DIR}/include)

# If there are any dependencies, link them here
target_link_libraries(ChronoKVS PUBLIC ChronoEmulator)

# For installation (optional)
install(TARGETS ChronoKVS DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)

# Testing (optional)
# enable_testing()
# add_subdirectory(tests)

# Examples (optional)
add_subdirectory(examples)

# Documentation (optional, add if you have custom CMake setup for docs)
# add_subdirectory(docs)
