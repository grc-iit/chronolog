cmake_minimum_required(VERSION 3.25)

#------------------------------------------------------------------------------
# Package Discovery Tests
#------------------------------------------------------------------------------
# These tests verify that Chronolog can be discovered and used by external
# projects using both CMake find_package() and pkg-config.
#
# These tests are always built (linking against the build tree version) and
# installed. They should be run AFTER installation to verify that the installed
# package can be discovered correctly.
#
# IMPORTANT: To run these tests after installation, set:
# 1. CMAKE_PREFIX_PATH to the install prefix (for CMake discovery)
# 2. PKG_CONFIG_PATH to <install-prefix>/chronolog/lib/pkgconfig (for pkg-config)
#------------------------------------------------------------------------------

#------------------------------------------------------------------------------
# Test 1: CMake find_package() Discovery
#------------------------------------------------------------------------------
# This test verifies that find_package(Chronolog) works correctly and that
# the Chronolog::chronolog_client target can be used.
# Note: We link against the build tree version here, but the test will verify
# that the installed version can be discovered when run after installation.
add_executable(test_cmake_discovery
    test_cmake_discovery.cpp
)

# Link against the build tree version (so it always compiles)
target_link_libraries(test_cmake_discovery PRIVATE chronolog_client)

# Include directories from build tree
target_include_directories(test_cmake_discovery PRIVATE
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

# Set properties for proper RPATH handling
set_target_properties(test_cmake_discovery PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# Add as a CTest test
enable_testing()
add_test(NAME PackageDiscovery_CMake COMMAND test_cmake_discovery)

# Install the test executable (useful for verifying installation)
chronolog_install_target(test_cmake_discovery)

#------------------------------------------------------------------------------
# Test 2: pkg-config Discovery
#------------------------------------------------------------------------------
# This test verifies that pkg-config can find chronolog.pc and that the
# compiler/linker flags work correctly.
# Note: We link against the build tree version here, but the test will verify
# that the installed version can be discovered when run after installation.
add_executable(test_pkgconfig_discovery
    test_pkgconfig_discovery.cpp
)

# For building, use the build tree version
target_include_directories(test_pkgconfig_discovery PRIVATE
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)
target_link_libraries(test_pkgconfig_discovery PRIVATE chronolog_client)

# Also need json-c (required dependency)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON-C REQUIRED json-c)
target_include_directories(test_pkgconfig_discovery PRIVATE ${JSON-C_INCLUDE_DIRS})
target_link_directories(test_pkgconfig_discovery PRIVATE ${JSON-C_LIBRARY_DIRS})
target_link_libraries(test_pkgconfig_discovery PRIVATE ${JSON-C_LIBRARIES})

# Set properties for proper RPATH handling
set_target_properties(test_pkgconfig_discovery PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# Add as a CTest test
add_test(NAME PackageDiscovery_pkgconfig COMMAND test_pkgconfig_discovery)

# Install the test executable (useful for verifying installation)
chronolog_install_target(test_pkgconfig_discovery)

