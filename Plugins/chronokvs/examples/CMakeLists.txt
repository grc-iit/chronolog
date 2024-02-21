cmake_minimum_required(VERSION 3.17)
project(ChronoKVS_Examples LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add executable for the sample app
add_executable(sample_app basic_functionality_example.cpp)

# Link the ChronoLogKVS library with the sample app
target_link_libraries(sample_app ChronoKVS)

# Use target_include_directories to include the ChronoLogKVS headers
target_include_directories(sample_app PRIVATE ${PROJECT_SOURCE_DIR}/../include)
