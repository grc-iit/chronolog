# ChronoKeeper: chronolog.v1.Journal and chronolog.internal.v1.Archive over a RAM journal and its WAL.
include(GNUInstallDirs)
include(GoogleTest)

add_library(chronolog_keeper_lib STATIC
    KeeperConfig.cpp
    adapter/ArchiveService.cpp
    adapter/Convert.cpp
    adapter/RouteRead.cpp
    adapter/JournalService.cpp
    archive/KeeperArchive.cpp
    journal/RamJournal.cpp
    membership/AcquisitionWatcher.cpp
    membership/ConfigMembership.cpp
    membership/RouteWatcher.cpp
    runtime/ClusterClient.cpp
    wal/FileSink.cpp
    wal/Record.cpp
    wal/WalJournal.cpp)
target_include_directories(chronolog_keeper_lib PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")
target_link_libraries(chronolog_keeper_lib
    PUBLIC chronolog_common_clock chronolog_contracts chronolog_common_rpc chronolog_common_worker chronolog_internal_proto
           chronolog_proto
           nlohmann_json::nlohmann_json Threads::Threads
    PRIVATE chronolog_warnings absl::crc32c absl::log)

add_executable(chrono_keeper main.cpp)
target_link_libraries(chrono_keeper PRIVATE chronolog_keeper_lib chronolog_warnings
    absl::log absl::log_initialize absl::log_globals)
install(TARGETS chrono_keeper RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT server)

add_executable(chronolog_keeper_tests
    tests/acquisition_watcher_test.cpp
    tests/clock_audit_client_test.cpp
    tests/cluster_client_test.cpp
    tests/config_test.cpp
    tests/fetch_hot_adapter_test.cpp
    tests/hlc_core_test.cpp
    tests/journal_adapter_test.cpp
    tests/journal_adapter_wire_test.cpp
    tests/keeper_archive_adapter_test.cpp
    tests/ram_journal_contract.cpp
    tests/ram_journal_test.cpp
    tests/route_read_test.cpp
    tests/route_watcher_test.cpp
    tests/system_clock_contract.cpp)
target_include_directories(chronolog_keeper_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/contract")
target_link_libraries(chronolog_keeper_tests PRIVATE chronolog_keeper_lib chronolog_warnings
    GTest::gtest GTest::gtest_main Threads::Threads absl::log_sink absl::log_sink_registry)
gtest_discover_tests(chronolog_keeper_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "keeper."
    DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)

add_executable(chronolog_keeper_wal_tests tests/wal_journal_contract.cpp tests/wal_journal_test.cpp tests/wal_crash_test.cpp)
target_include_directories(chronolog_keeper_wal_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/contract")
target_link_libraries(chronolog_keeper_wal_tests PRIVATE chronolog_keeper_lib chronolog_warnings
    GTest::gtest GTest::gtest_main Threads::Threads)
add_dependencies(chronolog_keeper_wal_tests chrono_keeper)
target_compile_definitions(chronolog_keeper_wal_tests PRIVATE CHRONOLOG_KEEPER_BINARY="$<TARGET_FILE:chrono_keeper>")
gtest_discover_tests(chronolog_keeper_wal_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "keeper."
    DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)

add_executable(chronolog_keeper_archive_tests tests/keeper_archive_test.cpp)
target_link_libraries(chronolog_keeper_archive_tests PRIVATE chronolog_keeper_lib chronolog_grapher_lib chronolog_warnings
    absl::log_sink_registry absl::log_sink GTest::gtest GTest::gtest_main Threads::Threads)
gtest_discover_tests(chronolog_keeper_archive_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "keeper."
    DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
