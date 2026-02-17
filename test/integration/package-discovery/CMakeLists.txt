cmake_minimum_required(VERSION 3.25)

#------------------------------------------------------------------------------
# Package Discovery Tests
#------------------------------------------------------------------------------
# These tests verify that Chronolog can be discovered and used by external
# projects. Configuration and build NEVER depend on the install prefix, so
# cmake .. succeeds even when the install prefix contains stale config/pkg-config
# files but missing libs/headers (partial install state).
#
# Test layout:
# 1. PackageDiscovery_CMake / PackageDiscovery_pkgconfig
#    - Built and linked against the BUILD TREE (chronolog_client target).
#    - Always runnable; verify that the client library works.
# 2. PackageDiscovery_Installed_CMake / PackageDiscovery_Installed_pkgconfig
#    - Run at CTest time via CMake -P scripts.
#    - Validate find_package(Chronolog) / pkg-config against the INSTALLED
#      prefix only when required artifacts exist (config + library + headers).
#    - If install is incomplete, tests are SKIPPED (exit 0) with a clear message.
#    - If install is complete but discovery fails, tests FAIL with a clear message.
#------------------------------------------------------------------------------

#------------------------------------------------------------------------------
# Test 1: CMake discovery (build-tree)
#------------------------------------------------------------------------------
# Always use the build tree so configure never fails due to install prefix state.
add_executable(test_cmake_discovery
    test_cmake_discovery.cpp
)
target_link_libraries(test_cmake_discovery PRIVATE chronolog_client)
target_include_directories(test_cmake_discovery PRIVATE
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

add_test(NAME PackageDiscovery_CMake COMMAND test_cmake_discovery)

# Install discovery test executables to tests/ (for optional install-tree runs)
chronolog_install_target(test_cmake_discovery DESTINATION tests)

#------------------------------------------------------------------------------
# Test 2: pkg-config discovery (build-tree)
#------------------------------------------------------------------------------
add_executable(test_pkgconfig_discovery
    test_pkgconfig_discovery.cpp
)
target_include_directories(test_pkgconfig_discovery PRIVATE
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)
target_link_libraries(test_pkgconfig_discovery PRIVATE chronolog_client)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON-C REQUIRED json-c)
target_include_directories(test_pkgconfig_discovery PRIVATE ${JSON-C_INCLUDE_DIRS})
target_link_directories(test_pkgconfig_discovery PRIVATE ${JSON-C_LIBRARY_DIRS})
target_link_libraries(test_pkgconfig_discovery PRIVATE ${JSON-C_LIBRARIES})

add_test(NAME PackageDiscovery_pkgconfig COMMAND test_pkgconfig_discovery)

# Install discovery test executables to tests/
chronolog_install_target(test_pkgconfig_discovery DESTINATION tests)

#------------------------------------------------------------------------------
# Installed-prefix discovery tests (run at CTest time, do not affect configure)
#------------------------------------------------------------------------------
# These tests run only when ctest is executed. They check that the install
# prefix (if fully installed) allows find_package(Chronolog) and pkg-config
# to work. They never block cmake .. or make.
# Pass the project's CMAKE_PREFIX_PATH so the discovery subproject can find
# the same dependencies (spdlog, json-c, etc.) that the main build used.
set(_discovery_script_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(_discovery_binary_dir "${CMAKE_CURRENT_BINARY_DIR}")
list(JOIN CMAKE_PREFIX_PATH ";" _extra_prefix_path)

# Pass spdlog_DIR if set (e.g. from Spack or -Dspdlog_DIR=...), so the discovery subproject can find spdlog
set(_spdlog_dir_arg "")
if(DEFINED spdlog_DIR AND NOT spdlog_DIR STREQUAL "")
    set(_spdlog_dir_arg "-DSPDLOG_DIR=${spdlog_DIR}")
endif()
# Note: If spdlog was found via CMAKE_PREFIX_PATH only (no spdlog_DIR), EXTRA_CMAKE_PREFIX_PATH must contain that path.

add_test(
    NAME PackageDiscovery_Installed_CMake
    COMMAND ${CMAKE_COMMAND}
        -DCHRONOLOG_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
        -DCMAKE_EXECUTABLE=${CMAKE_COMMAND}
        -DTEST_BINARY_DIR=${_discovery_binary_dir}
        -DEXTRA_CMAKE_PREFIX_PATH=${_extra_prefix_path}
        ${_spdlog_dir_arg}
        -P ${_discovery_script_dir}/RunPackageDiscoveryCMake.cmake
    WORKING_DIRECTORY ${_discovery_binary_dir}
)
set_tests_properties(PackageDiscovery_Installed_CMake PROPERTIES
    ENVIRONMENT "PKG_CONFIG_PATH=$ENV{PKG_CONFIG_PATH}"
)

add_test(
    NAME PackageDiscovery_Installed_pkgconfig
    COMMAND ${CMAKE_COMMAND}
        -DCHRONOLOG_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
        -DCMAKE_EXECUTABLE=${CMAKE_COMMAND}
        -DTEST_BINARY_DIR=${_discovery_binary_dir}
        -DEXTRA_CMAKE_PREFIX_PATH=${_extra_prefix_path}
        -P ${_discovery_script_dir}/RunPackageDiscoveryPkgConfig.cmake
    WORKING_DIRECTORY ${_discovery_binary_dir}
)
set_tests_properties(PackageDiscovery_Installed_pkgconfig PROPERTIES
    ENVIRONMENT "PKG_CONFIG_PATH=$ENV{PKG_CONFIG_PATH}"
)

# Allow installed discovery tests to be skipped when install is not present
# (script exits 0 on skip); no need to set PASS_REGULAR_EXPRESSION here
# since the script uses message(STATUS) for skip and message(FATAL_ERROR) for failure.
