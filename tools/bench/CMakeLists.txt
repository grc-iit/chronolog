find_package(benchmark CONFIG REQUIRED)

add_library(chronolog_bench_codec STATIC codec.cpp)
target_include_directories(chronolog_bench_codec PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chronolog_bench_codec PUBLIC chronolog_proto chronolog_internal_proto absl::crc32c
    PRIVATE chronolog_warnings)

add_executable(chronolog_bench_micro micro.cpp)
target_link_libraries(chronolog_bench_micro PRIVATE chronolog_bench_codec chronolog_common_clock chronolog_keeper_lib
    benchmark::benchmark chronolog_warnings Threads::Threads)

add_executable(chronolog_bench_load load.cpp)
target_link_libraries(chronolog_bench_load PRIVATE chronolog::client nlohmann_json::nlohmann_json chronolog_warnings
    Threads::Threads)

# Tier and replay read benches, run by hand.
add_executable(chronolog_archive_physical_bench physical_read_bench.cpp)
target_link_libraries(chronolog_archive_physical_bench PRIVATE
    chronolog_common_tier chronolog_warnings nlohmann_json::nlohmann_json)
add_executable(chronolog_archive_compaction_bench compaction_bench.cpp)
target_link_libraries(chronolog_archive_compaction_bench PRIVATE chronolog_common_tier chronolog_warnings)
add_executable(chronolog_archive_recovery_bench recovery_bench.cpp)
target_link_libraries(chronolog_archive_recovery_bench PRIVATE chronolog_common_tier chronolog_warnings)
add_executable(chronolog_player_archive_pool_bench archive_pool_bench.cpp)
target_link_libraries(chronolog_player_archive_pool_bench PRIVATE chronolog_player_replay chronolog_warnings)
add_executable(chronolog_player_compacted_read_bench compacted_read_bench.cpp)
target_link_libraries(chronolog_player_compacted_read_bench PRIVATE chronolog_player_replay chronolog_warnings)
add_executable(chronolog_player_tier_read_bench EXCLUDE_FROM_ALL tier_read_bench.cpp)
target_link_libraries(chronolog_player_tier_read_bench PRIVATE chronolog_player_lib chronolog_warnings)
# Route-change tail probe driven by route_measure.py.
add_executable(chronolog_player_route_tail_probe route_tail_probe.cpp)
target_link_libraries(chronolog_player_route_tail_probe PRIVATE chronolog_common_rpc chronolog_proto chronolog_warnings)

# Harness correctness only: reduced parameters, no thresholds. Full runs go through tools/bench/run.sh.
if(TARGET chrono_visor AND TARGET chrono_keeper AND TARGET chrono_grapher AND TARGET chrono_player)
    foreach(suite micro append replay tail archive)
        add_test(NAME bench.${suite}
            COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash ${CMAKE_CURRENT_SOURCE_DIR}/run.sh ${suite}
            --quick --binaries ${CMAKE_BINARY_DIR} --out ${CMAKE_BINARY_DIR}/bench-results/${suite})
        set_tests_properties(bench.${suite} PROPERTIES TIMEOUT 300 LABELS "bench")
        if(NOT suite STREQUAL "micro")
            set_tests_properties(bench.${suite} PROPERTIES RESOURCE_LOCK chronolog_stack)
        endif()
    endforeach()
endif()
