cmake_minimum_required(VERSION 3.25)

#message("ChronoKVS CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("ChronoKVS CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : ChronoKVS")

# Define the project and specify the required C++ standard
project(ChronoKVS LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(${CMAKE_SOURCE_DIR}/Plugins/chronoemulator)

# Source files for the library
set(SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/chronokvs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/chronokvs_mapper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/chronokvs_memorymanager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/chronokvs_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/chronolog_client.cpp
)

# Define the library target
add_library(ChronoKVS STATIC ${SOURCE_FILES})

# Use target_include_directories instead of include_directories
target_include_directories(ChronoKVS PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

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
