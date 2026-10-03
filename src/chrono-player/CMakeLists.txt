# Pure replay core: no gRPC beyond the contract types.
add_library(chrono_player_replay STATIC
    replay/CompletionPolicy.cpp
    replay/HotReplay.cpp
    replay/ReplayMerge.cpp)
target_include_directories(chrono_player_replay PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(chrono_player_replay PUBLIC chronolog_contracts chronolog_proto Threads::Threads chronolog_grapher_tier
    PRIVATE chronolog_warnings absl::log)

# gRPC clients and the Replay service.
add_library(chrono_player_lib STATIC
    PlayerConfig.cpp
    adapter/CatalogClient.cpp
    adapter/ClusterClient.cpp
    adapter/Convert.cpp
    adapter/ReplayService.cpp
    replay/KeeperHotSource.cpp
    replay/WriterDirectory.cpp)
target_link_libraries(chrono_player_lib
    PUBLIC chrono_player_replay chronolog_common_rpc chronolog_internal_proto chronolog_proto nlohmann_json::nlohmann_json
    PRIVATE chronolog_warnings absl::log)

add_executable(chrono_player main.cpp)
target_link_libraries(chrono_player PRIVATE chrono_player_lib chronolog_warnings
    absl::log absl::log_initialize absl::log_globals)

include(GoogleTest)

function(chrono_player_test name sources prefix)
    add_executable(${name} ${sources})
    target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/tests/contract")
    target_link_libraries(${name} PRIVATE chrono_player_lib chronolog_warnings GTest::gtest GTest::gtest_main)
    if("thread" IN_LIST CHRONOLOG_SANITIZERS AND CHRONOLOG_SETARCH)
        set_property(TARGET ${name} PROPERTY CROSSCOMPILING_EMULATOR
            "${CHRONOLOG_SETARCH};${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
    endif()
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 TEST_PREFIX "${prefix}" DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)
endfunction()

chrono_player_test(chrono_player_replay_merge_test tests/replay_merge_test.cpp "replay_merge.")
chrono_player_test(chrono_player_completion_policy_test tests/completion_policy_test.cpp "completion_policy.")
chrono_player_test(chrono_player_hot_replay_contract_test tests/hot_replay_contract.cpp "")
chrono_player_test(chrono_player_adapter_test "tests/replay_adapter_test.cpp;tests/player_config_test.cpp" "")

chrono_player_test(chrono_player_cold_replay_test "tests/cold_replay_test.cpp;tests/sequential_archive.cpp" "")
chrono_player_test(chrono_player_compacted_replay_test tests/compacted_replay_test.cpp "")

chrono_player_test(chrono_player_cluster_client_test tests/cluster_client_test.cpp "")
chrono_player_test(chrono_player_catalog_client_dns_test tests/catalog_client_dns_test.cpp "")

chrono_player_test(chrono_player_m8_replay_test "${PROJECT_SOURCE_DIR}/tests/contract/replay/m8_replay_test.cpp" "M8.")

target_link_libraries(chrono_player_m8_replay_test PRIVATE chronolog_keeper_lib)

if(CHRONOLOG_BUILD_VISOR)
    chrono_player_test(chrono_player_physical_policy_test tests/physical_policy_catalog_test.cpp "")
    target_link_libraries(chrono_player_physical_policy_test PRIVATE chronolog_visor_lib)
endif()

add_executable(chrono_player_route_tail_probe tests/route_tail_probe.cpp)
target_link_libraries(chrono_player_route_tail_probe PRIVATE chronolog_common_rpc chronolog_proto chronolog_warnings)

if(CHRONOLOG_BUILD_BENCH)
    add_executable(chrono_player_archive_pool_bench tests/archive_pool_bench.cpp)
    target_link_libraries(chrono_player_archive_pool_bench PRIVATE chrono_player_replay chronolog_warnings)
endif()
