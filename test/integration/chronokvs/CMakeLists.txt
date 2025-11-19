cmake_minimum_required(VERSION 3.25)

# Find required packages
find_package(Threads REQUIRED)

#message("ChronoKVS Integration Test CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("ChronoKVS Integration Test CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Build target: ChronoKVS Integration Test")

# Create the integration test executable
add_executable(chronokvs_integration_test
    chronokvs_integration_test.cpp
)

# Link with ChronoKVS library
target_link_libraries(chronokvs_integration_test
    chronokvs
    Threads::Threads
)

# Include directories
target_include_directories(chronokvs_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Plugins/chronokvs/include
)

# Set C++ standard
target_compile_features(chronokvs_integration_test PRIVATE cxx_std_17)

# Set target properties for proper RPATH handling (following project pattern)
set_target_properties(chronokvs_integration_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# Add test to CTest
add_test(NAME chronokvs_integration_test
    COMMAND chronokvs_integration_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Set test properties
set_tests_properties(chronokvs_integration_test PROPERTIES
    TIMEOUT 300
    LABELS "integration;chronokvs"
)

# Install the test executable using the project's standard function
chronolog_install_target(chronokvs_integration_test)
