add_library(chronolog_keeper_archive STATIC KeeperArchive.cpp)
target_include_directories(chronolog_keeper_archive PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(chronolog_keeper_archive PUBLIC chronolog_keeper_lib chronolog_keeper_wal
    chronolog_internal_proto PRIVATE absl::crc32c chronolog_warnings Threads::Threads)

include(GoogleTest)
add_executable(chronolog_keeper_archive_tests ../tests/keeper_archive_test.cpp)
target_include_directories(chronolog_keeper_archive_tests PRIVATE ../tests)
target_link_libraries(chronolog_keeper_archive_tests PRIVATE chronolog_keeper_archive
    GTest::gtest GTest::gtest_main chronolog_warnings Threads::Threads)
gtest_discover_tests(chronolog_keeper_archive_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
