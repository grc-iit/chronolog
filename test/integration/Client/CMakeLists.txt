cmake_minimum_required(VERSION 3.25)

find_package(OpenMP)
find_package(MPI)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ABT REQUIRED argobots)

# Option to install test binaries
option(CHRONOLOG_INSTALL_TESTS "Install test binaries" OFF)

#------------------------------------------------------------------------------
# Standard Client Integration Tests
#------------------------------------------------------------------------------
set(client_examples
    client_lib_connect_rpc_test
    client_lib_metadata_rpc_test
    client_lib_multi_pthread_test
    client_lib_thread_interdependency_test
    client_lib_multi_storytellers
    client_lib_story_reader
    client_reader_to_csv
)

foreach(client ${client_examples})
    add_executable(${client}
        ${client}.cpp
    )

    target_include_directories(${client} PRIVATE
        ${CMAKE_BINARY_DIR}/Client/include
        ${CMAKE_SOURCE_DIR}/chrono_common/include
        ${CMAKE_SOURCE_DIR}/Client/cpp/include
    )

    target_link_libraries(${client}
        PRIVATE
            chronolog_client
            pthread
            rt
    )

    set_target_properties(${client} PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        SKIP_BUILD_RPATH TRUE
    )
endforeach()

#------------------------------------------------------------------------------
# Argobots Client Integration Test
#------------------------------------------------------------------------------
add_executable(client_lib_multi_argobots_test
    client_lib_multi_argobots_test.cpp
)

target_include_directories(client_lib_multi_argobots_test PRIVATE
    ${CMAKE_BINARY_DIR}/Client/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${ABT_INCLUDE_DIRS}
)

target_link_directories(client_lib_multi_argobots_test
    PRIVATE
        ${ABT_LIBRARY_DIRS}
)

target_link_libraries(client_lib_multi_argobots_test
    PRIVATE
        chronolog_client
        ${ABT_LIBRARIES}
        pthread
        rt
)

set_target_properties(client_lib_multi_argobots_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# OpenMP Client Integration Test
#------------------------------------------------------------------------------
set(client_openmp
    client_lib_multi_openmp_test
)

foreach(client ${client_openmp})
    add_executable(${client}
        ${client}.cpp
    )

    target_include_directories(${client} PRIVATE
        ${CMAKE_BINARY_DIR}/Client/include
        ${CMAKE_SOURCE_DIR}/chrono_common/include
        ${CMAKE_SOURCE_DIR}/Client/cpp/include
    )

    target_link_libraries(${client}
        PRIVATE
            chronolog_client
            OpenMP::OpenMP_CXX
    )

    set_target_properties(${client} PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        SKIP_BUILD_RPATH TRUE
    )
endforeach()

#------------------------------------------------------------------------------
# Hybrid MPI + Argobots Client Integration Test
#------------------------------------------------------------------------------
add_executable(client_lib_hybrid_argobots_test
    client_lib_hybrid_argobots_test.cpp
)

target_include_directories(client_lib_hybrid_argobots_test PRIVATE
    ${CMAKE_BINARY_DIR}/Client/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${MPI_CXX_INCLUDE_DIRS}
    ${ABT_INCLUDE_DIRS}
)

target_link_directories(client_lib_hybrid_argobots_test
    PRIVATE
        ${ABT_LIBRARY_DIRS}
)

target_link_libraries(client_lib_hybrid_argobots_test
    PRIVATE
        chronolog_client
        ${MPI_CXX_LIBRARIES}
        ${ABT_LIBRARIES}
        pthread
        rt
)

set_target_properties(client_lib_hybrid_argobots_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Install test binaries to tests/ (only if enabled)
#------------------------------------------------------------------------------
if(CHRONOLOG_INSTALL_TESTS)
    foreach(client ${client_examples})
        chronolog_install_target(${client} DESTINATION tests)
    endforeach()
endif()
