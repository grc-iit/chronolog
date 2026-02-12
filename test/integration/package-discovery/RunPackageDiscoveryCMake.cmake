# RunPackageDiscoveryCMake.cmake
# CTest-time script: validate find_package(Chronolog) against the installed prefix.
# Expects: -DCHRONOLOG_INSTALL_PREFIX=... -DCMAKE_EXECUTABLE=... -DTEST_BINARY_DIR=...
# If the install prefix is incomplete (stale config but missing lib/headers), SKIP (exit 0).
# If the install prefix is complete but discovery fails, FAIL with a clear message.

if(NOT DEFINED CHRONOLOG_INSTALL_PREFIX OR CHRONOLOG_INSTALL_PREFIX STREQUAL "")
    message(FATAL_ERROR "Package discovery (installed CMake): CHRONOLOG_INSTALL_PREFIX is not set. "
                        "This test must be run via ctest.")
endif()
if(NOT DEFINED CMAKE_EXECUTABLE OR CMAKE_EXECUTABLE STREQUAL "")
    message(FATAL_ERROR "Package discovery (installed CMake): CMAKE_EXECUTABLE is not set.")
endif()
if(NOT DEFINED TEST_BINARY_DIR OR TEST_BINARY_DIR STREQUAL "")
    message(FATAL_ERROR "Package discovery (installed CMake): TEST_BINARY_DIR is not set.")
endif()

set(ROOT "${CHRONOLOG_INSTALL_PREFIX}/chronolog")
set(CONFIG_FILE "${ROOT}/lib/cmake/Chronolog/ChronologConfig.cmake")
set(TARGETS_FILE "${ROOT}/lib/cmake/Chronolog/ChronologTargets.cmake")
set(LIB_DIR "${ROOT}/lib")
set(INC_DIR "${ROOT}/include")

# Only treat as "installed" if required artifacts exist (config + library + headers)
if(NOT EXISTS "${CONFIG_FILE}")
    message(STATUS "Package discovery (installed CMake): SKIPPED - ChronologConfig.cmake not found at ${CONFIG_FILE}. "
                   "Install the package to run this test.")
    return()
endif()
if(NOT EXISTS "${TARGETS_FILE}")
    message(STATUS "Package discovery (installed CMake): SKIPPED - ChronologTargets.cmake not found. "
                   "Install the package to run this test.")
    return()
endif()

# GLOB for libchronolog_client.* (covers .so, .so.0, .dylib, etc.) without relying on CMAKE_* in -P mode
file(GLOB LIB_CANDIDATES "${LIB_DIR}/libchronolog_client.*" "${LIB_DIR}/libchronolog_client.so")
if(NOT LIB_CANDIDATES)
    message(STATUS "Package discovery (installed CMake): SKIPPED - chronolog_client library not found under ${LIB_DIR}. "
                   "Stale config/pkg-config may exist; install the package to run this test.")
    return()
endif()

set(KEY_HEADER "${INC_DIR}/chronolog_client.h")
if(NOT EXISTS "${KEY_HEADER}")
    message(STATUS "Package discovery (installed CMake): SKIPPED - headers not found (e.g. ${KEY_HEADER}). "
                   "Install the package to run this test.")
    return()
endif()

# Run a minimal project that uses find_package(Chronolog)
set(SUBDIR "${TEST_BINARY_DIR}/PackageDiscoveryCMakeBuild")
file(MAKE_DIRECTORY "${SUBDIR}")

# find_package(Chronolog) looks for <prefix>/lib/cmake/Chronolog; we install to <install_prefix>/chronolog/lib/cmake/Chronolog
# Prepend Chronolog prefix; append project's CMAKE_PREFIX_PATH so spdlog/json-c (and other deps) are findable
set(CHRONOLOG_PREFIX "${CHRONOLOG_INSTALL_PREFIX}/chronolog")
if(DEFINED EXTRA_CMAKE_PREFIX_PATH AND NOT EXTRA_CMAKE_PREFIX_PATH STREQUAL "")
    set(SUBPROJECT_PREFIX_PATH "${CHRONOLOG_PREFIX};${EXTRA_CMAKE_PREFIX_PATH}")
else()
    set(SUBPROJECT_PREFIX_PATH "${CHRONOLOG_PREFIX}")
endif()
set(CMAKE_LISTS "cmake_minimum_required(VERSION 3.25)\nproject(DiscoveryTest LANGUAGES CXX)\nset(CMAKE_PREFIX_PATH \"${SUBPROJECT_PREFIX_PATH}\")\nfind_package(Chronolog REQUIRED)\nadd_executable(tiny tiny.cpp)\ntarget_link_libraries(tiny PRIVATE Chronolog::chronolog_client)\n")
file(WRITE "${SUBDIR}/CMakeLists.txt" "${CMAKE_LISTS}")

set(TINY_CPP "#include <chronolog_client.h>\nint main() { (void)chronolog::ClientPortalServiceConf(); return 0; }\n")
file(WRITE "${SUBDIR}/tiny.cpp" "${TINY_CPP}")

# Pass through spdlog_DIR so the subproject can find spdlog (e.g. when main project used -Dspdlog_DIR=...)
set(SUBPROJECT_CMAKE_ARGS -B build -S . -DCMAKE_BUILD_TYPE=Release)
if(DEFINED SPDLOG_DIR AND NOT SPDLOG_DIR STREQUAL "")
    list(APPEND SUBPROJECT_CMAKE_ARGS -Dspdlog_DIR=${SPDLOG_DIR})
endif()

execute_process(
    COMMAND ${CMAKE_EXECUTABLE} ${SUBPROJECT_CMAKE_ARGS}
    WORKING_DIRECTORY "${SUBDIR}"
    RESULT_VARIABLE CONFIG_RESULT
    OUTPUT_VARIABLE CONFIG_OUT
    ERROR_VARIABLE CONFIG_ERR
)
if(CONFIG_RESULT)
    message(FATAL_ERROR "Package discovery (installed CMake): FAILED - find_package(Chronolog) failed for installed prefix.\n"
                        "Install prefix: ${CHRONOLOG_INSTALL_PREFIX}\n"
                        "stdout:\n${CONFIG_OUT}\nstderr:\n${CONFIG_ERR}")
endif()

execute_process(
    COMMAND ${CMAKE_EXECUTABLE} --build build
    WORKING_DIRECTORY "${SUBDIR}"
    RESULT_VARIABLE BUILD_RESULT
    OUTPUT_VARIABLE BUILD_OUT
    ERROR_VARIABLE BUILD_ERR
)
if(BUILD_RESULT)
    message(FATAL_ERROR "Package discovery (installed CMake): FAILED - build of discovery test failed.\n"
                        "stdout:\n${BUILD_OUT}\nstderr:\n${BUILD_ERR}")
endif()

message(STATUS "Package discovery (installed CMake): PASSED - find_package(Chronolog) works for installed prefix.")
