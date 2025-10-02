# ChronoKVS Integration Test CMakeLists.txt

# Find required packages
find_package(Threads REQUIRED)

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

# Compiler-specific options
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(chronokvs_integration_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

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

# Set target properties for proper RPATH handling
set_target_properties(chronokvs_integration_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# Install the test executable
install(
    TARGETS chronokvs_integration_test
    DESTINATION bin
)
