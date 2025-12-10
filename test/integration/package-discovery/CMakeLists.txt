cmake_minimum_required(VERSION 3.25)

#------------------------------------------------------------------------------
# Package Discovery Tests
#------------------------------------------------------------------------------
# These tests verify that Chronolog can be discovered and used by external
# projects using both CMake find_package() and pkg-config.
#
# IMPORTANT: These tests MUST be run AFTER installation. They use the installed
# package discovery mechanisms (CMake config files and pkg-config files) and
# will fail if the package has not been installed.
#
# The tests are configured to use:
# 1. CMAKE_PREFIX_PATH pointing to the install prefix (for CMake discovery)
# 2. PKG_CONFIG_PATH pointing to <install-prefix>/chronolog/lib/pkgconfig
#
# These tests verify that:
# - The installed CMake config files work correctly
# - The installed pkg-config file works correctly
# - Headers and libraries from the install directory are accessible
# - The package can be used by external projects
#------------------------------------------------------------------------------

# Get the install prefix and set up paths for package discovery
# Note: We use the install prefix so tests use the installed package files
set(CHRONOLOG_INSTALL_ROOT "${CMAKE_INSTALL_PREFIX}/chronolog")
set(CHRONOLOG_CMAKE_CONFIG_DIR "${CHRONOLOG_INSTALL_ROOT}/lib/cmake/Chronolog")
set(CHRONOLOG_PKGCONFIG_DIR "${CHRONOLOG_INSTALL_ROOT}/lib/pkgconfig")

# Check if the package has been installed
# The config files are only available after installation
set(CHRONOLOG_CONFIG_FILE "${CHRONOLOG_CMAKE_CONFIG_DIR}/ChronologConfig.cmake")
set(CHRONOLOG_PC_FILE "${CHRONOLOG_PKGCONFIG_DIR}/chronolog.pc")

if(EXISTS "${CHRONOLOG_CONFIG_FILE}" AND EXISTS "${CHRONOLOG_PC_FILE}")
    # Package is installed - we can test package discovery
    set(CHRONOLOG_PACKAGE_INSTALLED TRUE)
    message(STATUS "Package discovery tests: Using installed package at ${CMAKE_INSTALL_PREFIX}")
else()
    # Package not installed yet - tests will be skipped
    set(CHRONOLOG_PACKAGE_INSTALLED FALSE)
    message(WARNING "Package discovery tests: Package not installed yet. "
                    "These tests require installation first. Run 'make install' or 'cmake --install .' "
                    "then reconfigure to enable these tests.")
    message(STATUS "  Expected config file: ${CHRONOLOG_CONFIG_FILE}")
    message(STATUS "  Expected pkg-config file: ${CHRONOLOG_PC_FILE}")
endif()

# Set CMAKE_PREFIX_PATH to include the install prefix for find_package()
# This allows find_package(Chronolog) to locate the installed config files
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_INSTALL_PREFIX}")

# Set PKG_CONFIG_PATH to include the install pkgconfig directory
# This allows pkg-config to find the installed chronolog.pc file
set(ENV{PKG_CONFIG_PATH} "${CHRONOLOG_PKGCONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")

#------------------------------------------------------------------------------
# Test 1: CMake find_package() Discovery
#------------------------------------------------------------------------------
# This test verifies that find_package(Chronolog) works correctly and that
# the Chronolog::chronolog_client target from the installed package can be used.
# It uses the installed CMake config files, not the build tree.
if(CHRONOLOG_PACKAGE_INSTALLED)
    add_executable(test_cmake_discovery
        test_cmake_discovery.cpp
    )

    # Use find_package to discover the installed Chronolog package
    # This will find ChronologConfig.cmake from the install directory
    find_package(Chronolog REQUIRED)

    # Link against the installed Chronolog::chronolog_client target
    # This target provides all necessary include directories and link libraries
    # from the installation, not the build tree
    target_link_libraries(test_cmake_discovery PRIVATE Chronolog::chronolog_client)

    # Set properties for proper RPATH handling
    set_target_properties(test_cmake_discovery PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        SKIP_BUILD_RPATH TRUE
    )

    # Add as a CTest test
    add_test(NAME PackageDiscovery_CMake COMMAND test_cmake_discovery)

    # Install the test executable (useful for verifying installation)
    chronolog_install_target(test_cmake_discovery)
else()
    message(STATUS "Skipping CMake discovery test - package not installed")
endif()

#------------------------------------------------------------------------------
# Test 2: pkg-config Discovery
#------------------------------------------------------------------------------
# This test verifies that pkg-config can find the installed chronolog.pc file
# and that the compiler/linker flags work correctly.
# It uses the installed pkg-config file, not the build tree.
if(CHRONOLOG_PACKAGE_INSTALLED)
    add_executable(test_pkgconfig_discovery
        test_pkgconfig_discovery.cpp
    )

    # Use pkg-config to discover the installed chronolog package
    # This will find chronolog.pc from the install directory
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CHRONOLOG REQUIRED chronolog)

    # Use the flags from pkg-config (from the installed .pc file)
    # These point to the installed include and lib directories
    target_include_directories(test_pkgconfig_discovery PRIVATE ${CHRONOLOG_INCLUDE_DIRS})
    target_link_directories(test_pkgconfig_discovery PRIVATE ${CHRONOLOG_LIBRARY_DIRS})
    target_link_libraries(test_pkgconfig_discovery PRIVATE ${CHRONOLOG_LIBRARIES})

    # Also need json-c (required dependency from chronolog.pc)
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
else()
    message(STATUS "Skipping pkg-config discovery test - package not installed")
endif()

