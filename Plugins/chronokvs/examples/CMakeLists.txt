cmake_minimum_required(VERSION 3.25)
project(ChronoKVS_Examples LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add executable for the sample app
add_executable(ChronoKVS_example1 chronokvs_example1.cpp)

# Link the ChronoLogKVS library with the sample app
target_link_libraries(ChronoKVS_example1 ChronoKVS)

# Use target_include_directories to include the ChronoLogKVS headers
target_include_directories(ChronoKVS_example1 PRIVATE ${PROJECT_SOURCE_DIR}/../include)
