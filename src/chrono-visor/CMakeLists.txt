# ChronoVisor: Catalog (chronolog.v1) and Cluster (chronolog.internal.v1).

find_package(NuRaft CONFIG REQUIRED)

add_library(chronolog_visor_lib STATIC
    dynamic/MembershipState.cpp
    raft/DurableState.cpp
    raft/RaftMetadataStore.cpp
    VisorConfig.cpp
    adapter/CatalogService.cpp
    adapter/ClusterService.cpp
    adapter/Convert.cpp
    adapter/WorkerPool.cpp
    catalog/LeaseAuthority.cpp
    catalog/AcquisitionFeed.cpp
    catalog/InMemoryMetadataStore.cpp
    catalog/SqliteMetadataStore.cpp
    catalog/MembershipTables.cpp
    membership/StaticRouteMembership.cpp)
target_include_directories(chronolog_visor_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chronolog_visor_lib
    PUBLIC NuRaft::static_lib chronolog_contracts chronolog_common_rpc chronolog_internal_proto chronolog_proto
           unofficial::sqlite3::sqlite3 nlohmann_json::nlohmann_json Threads::Threads
    PRIVATE chronolog_warnings absl::log)

add_executable(chrono_visor main.cpp)
target_link_libraries(chrono_visor PRIVATE chronolog_visor_lib chronolog_warnings
    absl::log absl::log_initialize absl::log_globals)

# Contract suites are included as source into the instantiating test files, so the
# suite sources are reached through the contract directory.
add_executable(chronolog_visor_tests
    tests/sqlite_metadata_store_contract.cpp
    tests/static_route_membership_contract.cpp
    tests/acquisition_lease_test.cpp
    tests/sqlite_restart_test.cpp
    tests/catalog_adapter_test.cpp
    "${PROJECT_SOURCE_DIR}/tests/contract/catalog/catalog_adapter_test.cpp"
    tests/cluster_adapter_test.cpp
    tests/supersede_test.cpp
    tests/fence_end_to_end_test.cpp)
target_include_directories(chronolog_visor_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/contract")
target_link_libraries(chronolog_visor_tests PRIVATE
    chronolog_visor_lib chronolog_warnings GTest::gtest GTest::gtest_main)

include(GoogleTest)
# ThreadSanitizer needs ASLR disabled on kernels with a wide mmap_rnd_bits.
if("thread" IN_LIST CHRONOLOG_SANITIZERS AND CHRONOLOG_SETARCH)
    set_property(TARGET chronolog_visor_tests PROPERTY CROSSCOMPILING_EMULATOR
        "${CHRONOLOG_SETARCH};${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
endif()
gtest_discover_tests(chronolog_visor_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)

# Tiny Catalog client driven by tests/acceptance.sh against the real binary.
add_executable(visor_acceptance_client tests/acceptance_client.cpp)
target_link_libraries(visor_acceptance_client PRIVATE chronolog_proto chronolog_warnings)
set(visor_acceptance_launcher)
if("thread" IN_LIST CHRONOLOG_SANITIZERS AND CHRONOLOG_SETARCH)
    set(visor_acceptance_launcher "${CHRONOLOG_SETARCH}" "${CMAKE_HOST_SYSTEM_PROCESSOR}" -R)
endif()
add_test(NAME visor_binary_acceptance
         COMMAND ${visor_acceptance_launcher} timeout 120 bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance.sh"
                 $<TARGET_FILE:chrono_visor> $<TARGET_FILE:visor_acceptance_client>)

add_executable(chronolog_raft_tests tests/raft_metadata_store_contract.cpp)
target_include_directories(chronolog_raft_tests PRIVATE tests "${PROJECT_SOURCE_DIR}/tests/contract")
target_link_libraries(chronolog_raft_tests PRIVATE chronolog_visor_lib chronolog_warnings GTest::gtest_main)
gtest_discover_tests(chronolog_raft_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 45 RESOURCE_LOCK visor_raft_ports)

add_executable(visor_raft_acceptance_client tests/raft_acceptance_client.cpp)
target_link_libraries(visor_raft_acceptance_client PRIVATE chronolog_proto chronolog_warnings)
add_test(NAME visor_raft_binary_acceptance COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} timeout 120 bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/raft_acceptance.sh"
    $<TARGET_FILE:chrono_visor> $<TARGET_FILE:visor_raft_acceptance_client>)
set_tests_properties(visor_raft_binary_acceptance PROPERTIES TIMEOUT 125 RESOURCE_LOCK visor_raft_ports)

add_executable(chronolog_admin dynamic/admin.cpp)
target_link_libraries(chronolog_admin PRIVATE chronolog_common_rpc chronolog_internal_proto chronolog_warnings)

add_executable(chronolog_dynamic_membership_tests tests/dynamic_membership_test.cpp)
target_include_directories(chronolog_dynamic_membership_tests PRIVATE tests "${PROJECT_SOURCE_DIR}/tests/contract")
target_link_libraries(chronolog_dynamic_membership_tests PRIVATE chronolog_visor_lib chronolog_warnings GTest::gtest_main)
gtest_discover_tests(chronolog_dynamic_membership_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 120)

add_executable(chronolog_dynamic_cluster_tests tests/dynamic_cluster_test.cpp tests/floor_test.cpp)
target_include_directories(chronolog_dynamic_cluster_tests PRIVATE tests "${PROJECT_SOURCE_DIR}/tests/contract")
target_link_libraries(chronolog_dynamic_cluster_tests PRIVATE chronolog_visor_lib chronolog_keeper_lib chronolog_warnings GTest::gtest_main)
gtest_discover_tests(chronolog_dynamic_cluster_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 45 RESOURCE_LOCK visor_raft_ports)
