# RunPackageDiscoveryPkgConfig.cmake
# CTest-time script: validate pkg-config discovery against the installed prefix.
# Expects: -DCHRONOLOG_INSTALL_PREFIX=... -DCMAKE_EXECUTABLE=... -DTEST_BINARY_DIR=...
# If the install prefix is incomplete (stale .pc but missing lib/headers), SKIP (exit 0).
# If the install prefix is complete but discovery fails, FAIL with a clear message.

if(NOT DEFINED CHRONOLOG_INSTALL_PREFIX OR CHRONOLOG_INSTALL_PREFIX STREQUAL "")
    message(FATAL_ERROR "Package discovery (installed pkg-config): CHRONOLOG_INSTALL_PREFIX is not set. "
                        "This test must be run via ctest.")
endif()
if(NOT DEFINED CMAKE_EXECUTABLE OR CMAKE_EXECUTABLE STREQUAL "")
    message(FATAL_ERROR "Package discovery (installed pkg-config): CMAKE_EXECUTABLE is not set.")
endif()
if(NOT DEFINED TEST_BINARY_DIR OR TEST_BINARY_DIR STREQUAL "")
    message(FATAL_ERROR "Package discovery (installed pkg-config): TEST_BINARY_DIR is not set.")
endif()

set(ROOT "${CHRONOLOG_INSTALL_PREFIX}/chronolog")
set(PC_FILE "${ROOT}/lib/pkgconfig/chronolog.pc")
set(LIB_DIR "${ROOT}/lib")
set(INC_DIR "${ROOT}/include")

# Only treat as "installed" if required artifacts exist (.pc + library + headers)
if(NOT EXISTS "${PC_FILE}")
    message(STATUS "Package discovery (installed pkg-config): SKIPPED - chronolog.pc not found at ${PC_FILE}. "
                   "Install the package to run this test.")
    return()
endif()

# GLOB for libchronolog_client.* (covers .so, .so.0, .dylib, etc.) without relying on CMAKE_* in -P mode
file(GLOB LIB_CANDIDATES "${LIB_DIR}/libchronolog_client.*" "${LIB_DIR}/libchronolog_client.so")
if(NOT LIB_CANDIDATES)
    message(STATUS "Package discovery (installed pkg-config): SKIPPED - chronolog_client library not found under ${LIB_DIR}. "
                   "Stale config/pkg-config may exist; install the package to run this test.")
    return()
endif()

set(KEY_HEADER "${INC_DIR}/chronolog_client.h")
if(NOT EXISTS "${KEY_HEADER}")
    message(STATUS "Package discovery (installed pkg-config): SKIPPED - headers not found (e.g. ${KEY_HEADER}). "
                   "Install the package to run this test.")
    return()
endif()

# Run a minimal project that uses pkg_check_modules(chronolog)
# Pass EXTRA_CMAKE_PREFIX_PATH so the subproject can find dependencies (e.g. json-c) if needed
set(SUBDIR "${TEST_BINARY_DIR}/PackageDiscoveryPkgConfigBuild")
file(MAKE_DIRECTORY "${SUBDIR}")

if(DEFINED EXTRA_CMAKE_PREFIX_PATH AND NOT EXTRA_CMAKE_PREFIX_PATH STREQUAL "")
    set(PREFIX_PATH_LINE "set(CMAKE_PREFIX_PATH \"${EXTRA_CMAKE_PREFIX_PATH}\")\n")
else()
    set(PREFIX_PATH_LINE "")
endif()

set(CMAKE_LISTS
"cmake_minimum_required(VERSION 3.25)
project(DiscoveryTest LANGUAGES CXX)
${PREFIX_PATH_LINE}set(ENV{PKG_CONFIG_PATH} \"${ROOT}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}\")
find_package(PkgConfig REQUIRED)
pkg_check_modules(CHRONOLOG REQUIRED chronolog)
add_executable(tiny tiny.cpp)
target_include_directories(tiny PRIVATE \${CHRONOLOG_INCLUDE_DIRS})
target_link_directories(tiny PRIVATE \${CHRONOLOG_LIBRARY_DIRS})
target_link_libraries(tiny PRIVATE \${CHRONOLOG_LIBRARIES})
"
)
file(WRITE "${SUBDIR}/CMakeLists.txt" "${CMAKE_LISTS}")

set(TINY_CPP "#include <chronolog_client.h>\nint main() { (void)chronolog::ClientPortalServiceConf(); return 0; }\n")
file(WRITE "${SUBDIR}/tiny.cpp" "${TINY_CPP}")

execute_process(
    COMMAND ${CMAKE_EXECUTABLE} -B build -S . -DCMAKE_BUILD_TYPE=Release
    WORKING_DIRECTORY "${SUBDIR}"
    RESULT_VARIABLE CONFIG_RESULT
    OUTPUT_VARIABLE CONFIG_OUT
    ERROR_VARIABLE CONFIG_ERR
)
if(CONFIG_RESULT)
    message(FATAL_ERROR "Package discovery (installed pkg-config): FAILED - pkg_check_modules(chronolog) failed for installed prefix.\n"
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
    message(FATAL_ERROR "Package discovery (installed pkg-config): FAILED - build of discovery test failed.\n"
                        "stdout:\n${BUILD_OUT}\nstderr:\n${BUILD_ERR}")
endif()

message(STATUS "Package discovery (installed pkg-config): PASSED - pkg-config discovery works for installed prefix.")
