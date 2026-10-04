add_library(chronolog_grapher_server STATIC ArchiveService.cpp ClusterWorker.cpp GrapherConfig.cpp MigrationWorker.cpp TombstoneWatcher.cpp
    VisorClockAudit.cpp WorkerPool.cpp)
target_include_directories(chronolog_grapher_server PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(chronolog_grapher_server PUBLIC
    chronolog_common_tier chronolog_common_clock chronolog_common_rpc chronolog_internal_proto absl::crc32c
    PRIVATE chronolog_warnings nlohmann_json::nlohmann_json absl::log)

add_executable(chrono_grapher main.cpp)
target_link_libraries(chrono_grapher PRIVATE chronolog_grapher_server chronolog_warnings
    absl::log absl::log_initialize absl::log_globals)
include(GNUInstallDirs)
install(TARGETS chrono_grapher RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT server)

add_executable(chronolog_grapher_server_tests ../tests/archive_service_test.cpp ../tests/tombstone_watcher_test.cpp
    ../tests/visor_clock_audit_test.cpp ../tests/tier_worker_test.cpp)
target_link_libraries(chronolog_grapher_server_tests PRIVATE
    chronolog_grapher_server chronolog_warnings absl::log_sink absl::log_sink_registry GTest::gtest GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(chronolog_grapher_server_tests DISCOVERY_TIMEOUT 60
    DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)

add_executable(grapher_tier_archive_driver ../tests/tier_archive_driver.cpp)
target_link_libraries(grapher_tier_archive_driver PRIVATE chronolog_grapher_server chronolog_warnings)
add_test(NAME grapher.manifest_restart_migrated
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} python3 "${CMAKE_CURRENT_SOURCE_DIR}/../tests/manifest_restart_migrated.py"
    $<TARGET_FILE:chrono_grapher> $<TARGET_FILE:grapher_tier_archive_driver>)
set_tests_properties(grapher.manifest_restart_migrated PROPERTIES TIMEOUT 120 LABELS integration)
