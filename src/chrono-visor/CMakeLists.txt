# ChronoVisor: Catalog (chronolog.v1) and Cluster (chronolog.internal.v1).
# Legacy files under src/ and include/ are reference only and are not listed here.

add_library(chronolog_visor_lib STATIC
    VisorConfig.cpp
    adapter/CatalogService.cpp
    adapter/ClusterService.cpp
    adapter/Convert.cpp
    adapter/WorkerPool.cpp
    catalog/AcquisitionFeed.cpp
    catalog/InMemoryMetadataStore.cpp
    catalog/SqliteMetadataStore.cpp
    membership/StaticRouteMembership.cpp)
target_include_directories(chronolog_visor_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chronolog_visor_lib
    PUBLIC chronolog_contracts chronolog_internal_proto chronolog_proto
           unofficial::sqlite3::sqlite3 nlohmann_json::nlohmann_json Threads::Threads
    PRIVATE chronolog_warnings)

add_executable(chrono_visor main.cpp)
target_link_libraries(chrono_visor PRIVATE chronolog_visor_lib chronolog_warnings)

# Contract suites are included as source into the instantiating test files, so the
# suite sources are reached through the contract directory.
add_executable(chronolog_visor_tests
    tests/sqlite_metadata_store_contract.cpp
    tests/static_route_membership_contract.cpp
    tests/sqlite_restart_test.cpp
    tests/catalog_adapter_test.cpp
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
gtest_discover_tests(chronolog_visor_tests DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES)

# Tiny Catalog client driven by tests/acceptance.sh against the real binary.
add_executable(visor_acceptance_client tests/acceptance_client.cpp)
target_link_libraries(visor_acceptance_client PRIVATE chronolog_proto chronolog_warnings)
add_test(NAME visor_binary_acceptance
         COMMAND timeout 120 bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance.sh"
                 $<TARGET_FILE:chrono_visor> $<TARGET_FILE:visor_acceptance_client>)
