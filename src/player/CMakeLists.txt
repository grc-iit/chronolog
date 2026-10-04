# ChronoPlayer: chronolog.v1.Replay over the Keepers' hot windows and the tier chain.
include(GNUInstallDirs)
include(GoogleTest)

# Replay core with protobuf event sizing, no gRPC.
add_library(chronolog_player_replay STATIC
    adapter/EventConvert.cpp
    replay/CompletionPolicy.cpp
    replay/HotReplay.cpp
    replay/ReplayMerge.cpp)
target_include_directories(chronolog_player_replay PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")
target_link_libraries(chronolog_player_replay PUBLIC chronolog_contracts chronolog_proto Threads::Threads chronolog_common_tier
    PRIVATE chronolog_warnings absl::log)

# gRPC clients and the Replay service.
add_library(chronolog_player_lib STATIC
    PlayerConfig.cpp
    adapter/CatalogClient.cpp
    adapter/ClusterClient.cpp
    adapter/Convert.cpp
    adapter/ReplayService.cpp
    adapter/Await.cpp
    replay/KeeperHotSource.cpp
    replay/WriterDirectory.cpp)
target_link_libraries(chronolog_player_lib
    PUBLIC chronolog_player_replay chronolog_common_clock chronolog_common_rpc chronolog_internal_proto chronolog_proto
           nlohmann_json::nlohmann_json
    PRIVATE chronolog_warnings absl::log)

add_executable(chrono_player main.cpp)
target_link_libraries(chrono_player PRIVATE chronolog_player_lib chronolog_warnings
    absl::log absl::log_initialize absl::log_globals)
install(TARGETS chrono_player RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT server)

function(chronolog_player_test name sources)
    add_executable(${name} ${sources})
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/tests/contract")
    target_link_libraries(${name} PRIVATE chronolog_player_lib chronolog_warnings GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 TEST_PREFIX "player." DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES
        PROPERTIES TIMEOUT 30)
endfunction()

chronolog_player_test(chronolog_player_tests "tests/replay_adapter_test.cpp;tests/player_config_test.cpp")
chronolog_player_test(chronolog_player_replay_merge_tests tests/replay_merge_test.cpp)
chronolog_player_test(chronolog_player_completion_policy_tests tests/completion_policy_test.cpp)
chronolog_player_test(chronolog_player_hot_replay_contract_tests tests/hot_replay_contract.cpp)
chronolog_player_test(chronolog_player_cold_replay_tests "tests/cold_replay_test.cpp;tests/sequential_archive.cpp")
chronolog_player_test(chronolog_player_compacted_replay_tests tests/compacted_replay_test.cpp)
chronolog_player_test(chronolog_player_cluster_client_tests tests/cluster_client_test.cpp)
chronolog_player_test(chronolog_player_clock_audit_tests tests/clock_audit_test.cpp)
target_link_libraries(chronolog_player_clock_audit_tests PRIVATE absl::log_sink absl::log_sink_registry)
chronolog_player_test(chronolog_player_catalog_client_dns_tests tests/catalog_client_dns_test.cpp)
chronolog_player_test(chronolog_player_replay_scenarios_tests tests/replay_scenarios_test.cpp)
target_link_libraries(chronolog_player_replay_scenarios_tests PRIVATE chronolog_keeper_lib)

if(CHRONOLOG_BUILD_VISOR)
    chronolog_player_test(chronolog_player_physical_policy_tests tests/physical_policy_catalog_test.cpp)
    target_link_libraries(chronolog_player_physical_policy_tests PRIVATE chronolog_visor_lib)
endif()
