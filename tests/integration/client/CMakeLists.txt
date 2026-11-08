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
    client_replay_callback_test
    client_clientid_roundtrip_test
)

foreach(client ${client_standard_targets})
    add_executable(${client}
        ${client}.cpp
    )

    target_include_directories(${client} PRIVATE
        ${CMAKE_BINARY_DIR}/client/include
        ${CMAKE_SOURCE_DIR}/src/chrono-common/include
        ${CMAKE_SOURCE_DIR}/client/cpp/include
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
    ${CMAKE_BINARY_DIR}/client/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
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
    ${CMAKE_BINARY_DIR}/client/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
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
    ${CMAKE_BINARY_DIR}/client/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
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
    client_replay_callback_test
    client_clientid_roundtrip_test
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
    ReplayCallback
    ClientIdRoundtrip
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
    chronolog-test-client-replay-callback
    chronolog-test-client-clientid-roundtrip
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

find_package(GTest REQUIRED)
find_package(Thallium REQUIRED)
include(GoogleTest)
add_executable(client_acquire_public_probe client_acquire_public_probe.cpp)
target_include_directories(client_acquire_public_probe PRIVATE
    ${CMAKE_BINARY_DIR}/client/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include)
target_link_libraries(client_acquire_public_probe PRIVATE chronolog_client pthread)
set_target_properties(client_acquire_public_probe PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE)

add_executable(client_acquire_deadline_test
    client_acquire_deadline_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/KeeperRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/VisorAdminRuntime.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/VisorClientPortal.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/ClientRegistryManager.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/src/ChronicleMetaDirectory.cpp)
target_include_directories(client_acquire_deadline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-visor/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include)
target_link_libraries(client_acquire_deadline_test PRIVATE GTest::gtest_main chrono_rpc chrono_common chronolog_client thallium)
target_compile_definitions(client_acquire_deadline_test PRIVATE
    CHRONOLOG_PUBLIC_PROBE_PATH="$<TARGET_FILE:client_acquire_public_probe>")
add_dependencies(client_acquire_deadline_test client_acquire_public_probe)
# These native fixtures bind every endpoint to 127.0.0.1, including TCP.
gtest_discover_tests(client_acquire_deadline_test TEST_PREFIX "Integration_ClientAcquireDeadline_"
    PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE ENVIRONMENT "FI_SOCKETS_IFACE=lo;FI_TCP_IFACE=lo")

add_executable(client_metadata_deadline_test client_metadata_deadline_test.cpp)
target_include_directories(client_metadata_deadline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include ${CMAKE_SOURCE_DIR}/client/cpp/include ${CMAKE_SOURCE_DIR}/client/cpp/src)
target_link_libraries(client_metadata_deadline_test PRIVATE GTest::gtest_main chronolog_client chrono_rpc thallium)
gtest_discover_tests(client_metadata_deadline_test TEST_PREFIX "Integration_ClientMetadataDeadline_"
    PROPERTIES TIMEOUT 30)

target_include_directories(client_metadata_deadline_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)
target_compile_definitions(client_metadata_deadline_test PRIVATE CHRONOLOG_NATIVE_PEER_PATH="$<TARGET_FILE:client_native_peer>")
add_dependencies(client_metadata_deadline_test client_native_peer)

add_executable(client_append_deadline_test client_append_deadline_test.cpp)
target_include_directories(client_append_deadline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/client/cpp/src ${CMAKE_SOURCE_DIR}/tests/support)
target_link_libraries(client_append_deadline_test PRIVATE GTest::gtest_main chronolog_client chrono_rpc thallium)
target_compile_definitions(client_append_deadline_test PRIVATE CHRONOLOG_NATIVE_PEER_PATH="$<TARGET_FILE:client_native_peer>")
add_dependencies(client_append_deadline_test client_native_peer)
gtest_discover_tests(client_append_deadline_test TEST_PREFIX "Integration_ClientAppendDeadline_"
    PROPERTIES TIMEOUT 30)

# Each case starts a cold native child and uses the actual client ABI owner.
add_executable(client_replay_deadline_test client_replay_deadline_test.cpp)
target_include_directories(client_replay_deadline_test PRIVATE ${CMAKE_SOURCE_DIR}/client/cpp/src)
target_link_libraries(client_replay_deadline_test PRIVATE chronolog_client chrono_common chrono_rpc thallium)
foreach(transport IN ITEMS sockets tcp)
    foreach(scenario IN ITEMS submission original map-lock cleanup-lock cleanup-drop admission unknown-capacity last-id response-before-ack bad-ack negative-ack drop-late drop-output-late duplicate-owner callback-active callback-late callback-throw callback-control callback-append)
        add_test(NAME Integration_ClientReplayDeadline_${transport}_${scenario}
            COMMAND client_replay_deadline_test ofi+${transport} ${scenario})
        set_tests_properties(Integration_ClientReplayDeadline_${transport}_${scenario}
            PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
    endforeach()
endforeach()
foreach(transport IN ITEMS sockets tcp)
    add_test(NAME Integration_ClientReplayDeadline_${transport}_shipped-default
        COMMAND client_replay_deadline_test ofi+${transport} shipped-default)
    set_tests_properties(Integration_ClientReplayDeadline_${transport}_shipped-default
        PROPERTIES TIMEOUT 210 RUN_SERIAL TRUE ENVIRONMENT "FI_SOCKETS_IFACE=lo;FI_TCP_IFACE=lo")
endforeach()
