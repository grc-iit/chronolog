add_library(chronolog_keeper_wal STATIC FileSink.cpp Record.cpp WalJournal.cpp)
target_include_directories(chronolog_keeper_wal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(chronolog_keeper_wal PUBLIC chronolog_keeper_journal
    PRIVATE chronolog_internal_proto chronolog_proto absl::crc32c chronolog_warnings Threads::Threads)

include(GoogleTest)
add_executable(chronolog_keeper_wal_tests
    ../tests/wal_journal_contract.cpp ../tests/wal_journal_test.cpp ../tests/wal_crash_test.cpp)
target_include_directories(chronolog_keeper_wal_tests PRIVATE ../tests)
target_link_libraries(chronolog_keeper_wal_tests PRIVATE chronolog_keeper_wal chronolog_keeper_lib
    GTest::gtest GTest::gtest_main chronolog_warnings Threads::Threads)
gtest_discover_tests(chronolog_keeper_wal_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)

add_dependencies(chronolog_keeper_wal_tests chrono_keeper)
target_compile_definitions(chronolog_keeper_wal_tests PRIVATE
    CHRONOLOG_KEEPER_BINARY="$<TARGET_FILE:chrono_keeper>")
