cmake_minimum_required(VERSION 3.25)

find_package(OpenMP)
find_package(MPI)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ABT REQUIRED argobots)

#------------------------------------------------------------------------------
# Client integration tests (standard: client_element_test.cpp)
#------------------------------------------------------------------------------
set(client_standard_targets
    client_connect_rpc_test
    client_metadata_rpc_test
    client_multi_pthread_test
    client_thread_interdependency_test
    client_multi_storytellers_test
    client_story_reader_test
)

foreach(client ${client_standard_targets})
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
# Client integration test: Argobots
#------------------------------------------------------------------------------
add_executable(client_multi_argobots_test
    client_multi_argobots_test.cpp
)

target_include_directories(client_multi_argobots_test PRIVATE
    ${CMAKE_BINARY_DIR}/Client/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${ABT_INCLUDE_DIRS}
)

target_link_directories(client_multi_argobots_test
    PRIVATE
        ${ABT_LIBRARY_DIRS}
)

target_link_libraries(client_multi_argobots_test
    PRIVATE
        chronolog_client
        ${ABT_LIBRARIES}
        pthread
        rt
)

set_target_properties(client_multi_argobots_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Client integration test: OpenMP
#------------------------------------------------------------------------------
add_executable(client_multi_openmp_test
    client_multi_openmp_test.cpp
)

target_include_directories(client_multi_openmp_test PRIVATE
    ${CMAKE_BINARY_DIR}/Client/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_libraries(client_multi_openmp_test
    PRIVATE
        chronolog_client
        OpenMP::OpenMP_CXX
)

set_target_properties(client_multi_openmp_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Client integration test: Hybrid MPI + Argobots
#------------------------------------------------------------------------------
add_executable(client_hybrid_argobots_test
    client_hybrid_argobots_test.cpp
)

target_include_directories(client_hybrid_argobots_test PRIVATE
    ${CMAKE_BINARY_DIR}/Client/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${MPI_CXX_INCLUDE_DIRS}
    ${ABT_INCLUDE_DIRS}
)

target_link_directories(client_hybrid_argobots_test
    PRIVATE
        ${ABT_LIBRARY_DIRS}
)

target_link_libraries(client_hybrid_argobots_test
    PRIVATE
        chronolog_client
        ${MPI_CXX_LIBRARIES}
        ${ABT_LIBRARIES}
        pthread
        rt
)

set_target_properties(client_hybrid_argobots_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Register all Client integration executables as MANUAL CTest
#------------------------------------------------------------------------------
set(client_all_targets
    client_connect_rpc_test
    client_metadata_rpc_test
    client_multi_pthread_test
    client_thread_interdependency_test
    client_multi_storytellers_test
    client_story_reader_test
    client_multi_argobots_test
    client_multi_openmp_test
    client_hybrid_argobots_test
)
set(client_ctest_names
    ConnectRpc
    MetadataRpc
    MultiPthread
    ThreadInterdependency
    MultiStorytellers
    StoryReader
    MultiArgobots
    MultiOpenmp
    HybridArgobots
)
set(client_install_names
    chronolog-test-client-connect-rpc
    chronolog-test-client-metadata-rpc
    chronolog-test-client-multi-pthread
    chronolog-test-client-thread-interdependency
    chronolog-test-client-multi-storytellers
    chronolog-test-client-story-reader
    chronolog-test-client-multi-argobots
    chronolog-test-client-multi-openmp
    chronolog-test-client-hybrid-argobots
)
list(LENGTH client_all_targets _n)
math(EXPR _n "${_n} - 1")
foreach(_i RANGE ${_n})
    list(GET client_all_targets ${_i} _target)
    list(GET client_ctest_names ${_i} _name)
    list(GET client_install_names ${_i} _install_name)
    set_target_properties(${_target} PROPERTIES OUTPUT_NAME ${_install_name})
    add_test(NAME Integration_Client_${_name} COMMAND ${_target})
    set_tests_properties(Integration_Client_${_name} PROPERTIES MANUAL TRUE)
endforeach()
unset(_target)
unset(_name)
unset(_install_name)
unset(_i)
unset(_n)

#------------------------------------------------------------------------------
# Install test binaries to tests/ (only if enabled)
#------------------------------------------------------------------------------
if(CHRONOLOG_INSTALL_TESTS)
    foreach(client ${client_all_targets})
        chronolog_install_target(${client} DESTINATION tests)
    endforeach()
endif()
