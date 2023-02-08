cmake_minimum_required(VERSION 3.17)

# Set a consistent MACOSX_RPATH default across all CMake versions.
if(NOT DEFINED CMAKE_MACOSX_RPATH)
    set(CMAKE_MACOSX_RPATH 0)
endif()

project(ChronoLog LANGUAGES CXX)

#------------------------------------------------------------------------------
# Version information
#------------------------------------------------------------------------------
set(CHRONOLOG_VERSION_MAJOR "0")
set(CHRONOLOG_VERSION_MINOR "1")
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
if(NOT CHRONOLOG_INSTALL_BIN_DIR)
    set(CHRONOLOG_INSTALL_BIN_DIR ${CMAKE_INSTALL_PREFIX}/bin)
endif()
if(NOT CHRONOLOG_INSTALL_LIB_DIR)
    set(CHRONOLOG_INSTALL_LIB_DIR ${CMAKE_INSTALL_PREFIX}/lib)
endif()
if(NOT CHRONOLOG_INSTALL_INCLUDE_DIR)
    set(CHRONOLOG_INSTALL_INCLUDE_DIR ${CMAKE_INSTALL_PREFIX}/include)
endif()
if(NOT CHRONOLOG_INSTALL_DATA_DIR)
    set(CHRONOLOG_INSTALL_DATA_DIR ${CMAKE_INSTALL_PREFIX}/share)
endif()

#------------------------------------------------------------------------------
# Disallow in-source build. This checks that there is a defined and separate /build folder
#------------------------------------------------------------------------------
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(FATAL_ERROR
            "ChronoLog requires an out of source build. "
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
# TODO Should we consider the posibility of a user passing their own fflags through CMAKE_CXX_FLAGS
#   Encapsulate everything around if(NOT CMAKE_CXX_FLAGS)
#   Check Hermes for hwo they delete flags automatically
#if(SET_WERROR)
#    add_compile_options(-Wall)
#    add_compile_options(-Wextra)
#    add_compile_options(-Werror)
#else()
#    add_compile_options(-Wall)
#    add_compile_options(-Wextra)
#endif()

# Change some compilation flags depending on the build type
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message("Compiling ChronoLog in Debug Mode")
#    add_compile_options(-g3)
    add_compile_options(-O0 -fiopenmp)
    #set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O0 -g")

    #if (HERMES_USE_ADDRESS_SANITIZER)
    #    add_compile_options(-fsanitize=address)
    #endif()

    #if (HERMES_USE_THREAD_SANITIZER)
    #    add_compile_options(-fsanitize=thread)
    #endif()

    #if (HERMES_USE_THREAD_SANITIZER AND HERMES_USE_ADDRESS_SANITIZER)
    #    message(FATAL_ERROR "Cannont use -fsanitize=address and -fsanitize=thread "
    #            "at the same time")
    #endif()
else()    
    message("Compiling ChronoLog in release Mode")
    add_compile_options(-O3 -fiopenmp -lm)
    #add_compile_options(-Wno-switch-unreachable -Wno-return-type)
endif()

#------------------------------------------------------------------------------
# Error Checking on incompatible flags.
#------------------------------------------------------------------------------
if(CHRONOLOG_ENABLE_COVERAGE)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(FATAL_ERROR  "Use code coverage with debug mode")
    endif()
endif()


#------------------------------------------------------------------------------
# Transform flags from boolean to meaningfull usages.
#------------------------------------------------------------------------------
if(BUILD_SHARED_LIBS)
    set(CHRONOLOG_LIBTYPE SHARED)
else()
    set(CHRONOLOG_LIBTYPE STATIC)
endif()


#-----------------------------------------------------------------------------
# Dependencies common to all subdirectories
#-----------------------------------------------------------------------------
find_package(MPI REQUIRED COMPONENTS CXX)
find_package(Boost REQUIRED)
find_package(Thallium REQUIRED)
#find_library(ROCKSDB_LIB rocksdb)
#if(NOT ROCKSDB_LIB)
#    message(FATAL_ERROR "rocksdb is required")
#else()
#    find_path(ROCKDSB_INCLUDE rocksdb)
#    MESSAGE(STATUS "Found rocksdb at: ${ROCKSDB_INCLUDE}")
#endif()
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
        ${CMAKE_CURRENT_SOURCE_DIR} )

#-----------------------------------------------------------------------------
# Source
#-----------------------------------------------------------------------------
if(CHRONOLOG_BUILD_TESTING)
    message(STATUS "Tests enable")
    include(CTest)
endif()

# External header only libraries
add_subdirectory(external_libs)

add_subdirectory(tools)

# ChronoVisor
add_subdirectory(ChronoAPI)

# ChronoVisor
add_subdirectory(ChronoVisor)

# ChronoLog client library
add_subdirectory(Client)

# custom
add_custom_target(make_all cd ${CMAKE_BINARY_DIR} && make)

#Tests
if(CHRONOLOG_BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()
