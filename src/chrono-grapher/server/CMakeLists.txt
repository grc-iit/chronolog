add_library(chronolog_grapher_server STATIC ArchiveService.cpp ClusterWorker.cpp GrapherConfig.cpp TombstoneWatcher.cpp
    VisorClockAudit.cpp WorkerPool.cpp)
target_include_directories(chronolog_grapher_server PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(chronolog_grapher_server PUBLIC
    chronolog_grapher_tier chronolog_common_clock chronolog_common_rpc chronolog_internal_proto absl::crc32c
    PRIVATE chronolog_warnings nlohmann_json::nlohmann_json absl::log)

add_executable(chrono_grapher main.cpp)
target_link_libraries(chrono_grapher PRIVATE chronolog_grapher_server chronolog_warnings
    absl::log absl::log_initialize absl::log_globals)

add_executable(chronolog_grapher_server_tests ../tests/archive_service_test.cpp ../tests/tombstone_watcher_test.cpp
    ../tests/visor_clock_audit_test.cpp)
target_link_libraries(chronolog_grapher_server_tests PRIVATE
    chronolog_grapher_server chronolog_warnings absl::log_sink absl::log_sink_registry GTest::gtest GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(chronolog_grapher_server_tests DISCOVERY_TIMEOUT 60
    DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)
