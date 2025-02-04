cmake_minimum_required(VERSION 3.25)

# Set a consistent MACOSX_RPATH default across all CMake versions.
if(NOT DEFINED CMAKE_MACOSX_RPATH)
    set(CMAKE_MACOSX_RPATH 0)
endif()

project(ChronoLog LANGUAGES CXX C)

#------------------------------------------------------------------------------
# Version information
#------------------------------------------------------------------------------
set(CHRONOLOG_VERSION_MAJOR "0")
set(CHRONOLOG_VERSION_MINOR "7")
set(CHRONOLOG_VERSION_PATCH "0")
set(CHRONOLOG_PACKAGE "ChronoLog")
set(CHRONOLOG_PACKAGE_NAME "CHRONOLOG")
set(CHRONOLOG_PACKAGE_VERSION "${CHRONOLOG_VERSION_MAJOR}.${CHRONOLOG_VERSION_MINOR}.${CHRONOLOG_VERSION_PATCH}")
set(CHRONOLOG_PACKAGE_VERSION_MAJOR "${CHRONOLOG_VERSION_MAJOR}.${CHRONOLOG_VERSION_MINOR}")
set(CHRONOLOG_PACKAGE_VERSION_MINOR "${CHRONOLOG_VERSION_PATCH}")
set(CHRONOLOG_PACKAGE_STRING "${CHRONOLOG_PACKAGE_NAME} ${CHRONOLOG_PACKAGE_VERSION}")
set(CHRONOLOG_PACKAGE_TARNAME "${CHRONOLOG_PACKAGE}")

#------------------------------------------------------------------------------
# Setup install and output Directories
#------------------------------------------------------------------------------
# Validate CMAKE_BUILD_TYPE
if( NOT DEFINED CMAKE_BUILD_TYPE OR NOT (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "Release"))
    message(FATAL_ERROR "Please, define CMAKE_BUILD_TYPE that is either Debug or Release")
endif()

message("CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")

if(NOT DEFINED INSTALL_DIR)
    set(INSTALL_DIR "/home/$ENV{USER}/chronolog" CACHE PATH "Installation directory")
endif()

set(CMAKE_INSTALL_PREFIX ${INSTALL_DIR}/${CMAKE_BUILD_TYPE})
message("Installation Directory CMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}")

set(CHRONOLOG_INSTALL_INCLUDE_DIR ${CMAKE_INSTALL_PREFIX}/include)
set(CHRONOLOG_INSTALL_LIB_DIR ${CMAKE_INSTALL_PREFIX}/lib)
set(CHRONOLOG_INSTALL_BIN_DIR ${CMAKE_INSTALL_PREFIX}/bin)
set(CHRONOLOG_INSTALL_CONF_DIR ${CMAKE_INSTALL_PREFIX}/conf)

#------------------------------------------------------------------------------
# Disallow in-source build. This checks that there is a defined and separate /build folder
#------------------------------------------------------------------------------
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(FATAL_ERROR
        "ChronoLog requires an out of source build."
        "Please create a separate binary directory and run CMake there.")
endif()

#------------------------------------------------------------------------------
# Set CXX Standard if one was not defined
#------------------------------------------------------------------------------
if(NOT "${CMAKE_CXX_STANDARD}")
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

#find_package(OpenMP)
#find_package(IntelDPCPP REQUIRED)

#------------------------------------------------------------------------------
# Options for compilations
#------------------------------------------------------------------------------
option(SET_WERROR "Elevate warnings to errors" OFF)
option(BUILD_SHARED_LIBS "Build with shared libraries." ON)
option(CHRONOLOG_ENABLE_COVERAGE "Enable code coverage." OFF)
option(CHRONOLOG_USE_ADDRESS_SANITIZER "Enable -fsanitize=address in Debug builds" ON)
option(CHRONOLOG_USE_THREAD_SANITIZER "Enable -fsanitize=thread in Debug builds" OFF)
option(CHRONOLOG_BUILD_TESTING "Build the testing tree." ON)
option(CHRONOLOG_ENABLE_DOXYGEN "Enable Doxygen documentation generation." OFF)

#------------------------------------------------------------------------------
# Define the compiler flags
#------------------------------------------------------------------------------
# TODO Should we consider the possibility of a user passing their own flags through CMAKE_CXX_FLAGS
#   Encapsulate everything around if(NOT CMAKE_CXX_FLAGS)
#   Check Hermes for how they delete flags automatically
#if(SET_WERROR)
#    add_compile_options(-Wall)
#    add_compile_options(-Wextra)
#    add_compile_options(-Werror)
#else()
#    add_compile_options(-Wall)
#    add_compile_options(-Wextra)
#endif()

#set(CMAKE_BUILD_TYPE "Debug")
# Change some compilation flags depending on the build type
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message("Compiling ChronoLog in Debug Mode")
    add_compile_options(-O0)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -O0 -g")

    #if (CHRONOLOG_USE_ADDRESS_SANITIZER)
    #    add_compile_options(-fsanitize=address)
    #endif()

    #if (CHRONOLOG_USE_THREAD_SANITIZER)
    #    add_compile_options(-fsanitize=thread)
    #endif()

    #if (CHRONOLOG_USE_THREAD_SANITIZER AND CHRONOLOG_USE_ADDRESS_SANITIZER)
    #    message(FATAL_ERROR "Cannot use -fsanitize=address and -fsanitize=thread "
    #            "at the same time")
    #endif()
else()
    message("Compiling ChronoLog in Release Mode")
    add_compile_options(-O3 -lm)
    #add_compile_options(-Wno-switch-unreachable -Wno-return-type)
endif()

#------------------------------------------------------------------------------
# Error Checking on incompatible flags.
#------------------------------------------------------------------------------
if(CHRONOLOG_ENABLE_COVERAGE)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(FATAL_ERROR "Use code coverage with debug mode")
    endif()
endif()

#------------------------------------------------------------------------------
# Transform flags from boolean to meaningful usages.
#------------------------------------------------------------------------------
if(BUILD_SHARED_LIBS)
    set(CHRONOLOG_LIBTYPE SHARED)
else()
    set(CHRONOLOG_LIBTYPE STATIC)
endif()

#-----------------------------------------------------------------------------
# Dependencies common to all subdirectories
#-----------------------------------------------------------------------------

#-----------------------------------------------------------------------------
# Coverage
#-----------------------------------------------------------------------------
if(CHRONOLOG_ENABLE_COVERAGE)
    set(COVERAGE_FLAGS "-fprofile-arcs -ftest-coverage" CACHE STRING
        "Flags to the coverage program to perform coverage inspection")
    mark_as_advanced(COVERAGE_FLAGS)

    macro(set_coverage_flags target)
        set_target_properties(${target}
            PROPERTIES
            COMPILE_FLAGS ${COVERAGE_FLAGS}
            LINK_FLAGS ${COVERAGE_FLAGS}
        )
    endmacro()
endif()

#-----------------------------------------------------------------------------
# Documentation
#-----------------------------------------------------------------------------

if(CHRONOLOG_ENABLE_DOXYGEN)
    message(STATUS "Doxygen enable, generating documentation")
    find_package(Doxygen REQUIRED)
    include(UseDoxygenDoc)

    add_doxygen_doc(
        BUILD_DIR
        ${CMAKE_CURRENT_BINARY_DIR}/_build
        DOXY_FILE
        ${CMAKE_CURRENT_SOURCE_DIR}/doc/Doxyfile.in
        TARGET_NAME
        dox
        COMMENT
        "HTML documentation"
    )
endif()

#-----------------------------------------------------------------------------
# Linting
#-----------------------------------------------------------------------------
add_custom_target(lint COMMAND cpplint --recursive
    --exclude=${CMAKE_CURRENT_SOURCE_DIR}/build
    --exclude=${CMAKE_CURRENT_SOURCE_DIR}/cmake-build-debug
    --exclude=${CMAKE_CURRENT_SOURCE_DIR}/external_libs
    --exclude=${CMAKE_CURRENT_SOURCE_DIR}/doc
    --exclude=${CMAKE_CURRENT_SOURCE_DIR}/CI
    ${CMAKE_CURRENT_SOURCE_DIR})

#-----------------------------------------------------------------------------
# Source
#-----------------------------------------------------------------------------
if(CHRONOLOG_BUILD_TESTING)
    message(STATUS "Tests enable")
    include(CTest)
endif()

#-----------------------------------------------------------------------------
# ChronoLog Components
#-----------------------------------------------------------------------------
add_subdirectory(Client)
add_subdirectory(ChronoVisor)
add_subdirectory(ChronoKeeper)
add_subdirectory(ChronoGrapher)
add_subdirectory(ChronoPlayer)
add_subdirectory(ChronoStore)
if(CHRONOLOG_ENABLE_PYTHON_BINDINGS)
message("Python binding enabled")
add_subdirectory(python_client)
endif()
if(CHRONOLOG_BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()

#-----------------------------------------------------------------------------
# Custom target to invoke 'make' in the build directory
#-----------------------------------------------------------------------------
add_custom_target(make_all cd ${CMAKE_BINARY_DIR} && make)
