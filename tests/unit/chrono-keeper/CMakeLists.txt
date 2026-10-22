cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
find_package(Thallium REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# chrono_keeper unit tests
#------------------------------------------------------------------------------

# KeeperChunkRetentionStore (GTest) — header-only store, no network needed
add_executable(keeper_retention_store_test
    chrono_keeper_retention_store_test.cpp
)

target_include_directories(keeper_retention_store_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_retention_store_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(keeper_retention_store_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-retention-store)
# the concurrency test would show a lifetime bug as a hang as often as a crash
gtest_discover_tests(keeper_retention_store_test TEST_PREFIX "Unit_ChronoKeeper_" PROPERTIES TIMEOUT 60)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_retention_store_test DESTINATION tests)
endif()

# KeeperChunkRetentionStore retention cap (GTest) — its own binary because the
# chrono logger's level is process-wide and these tests need warnings on
add_executable(keeper_retention_cap_test
    chrono_keeper_retention_cap_test.cpp
)

target_include_directories(keeper_retention_cap_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_retention_cap_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(keeper_retention_cap_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-retention-cap)
gtest_discover_tests(keeper_retention_cap_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_retention_cap_test DESTINATION tests)
endif()

# Keeper extraction-chain disposal seam (GTest) — dispose_chunk routes drain
# outcomes into the retention store; shim mode (expects_watermarks()==false)
# frees on ack. Pulls DualEndpointChunkExtractorRDMA.cpp directly because it is
# only compiled into the chrono_keeper executable.
add_executable(keeper_extraction_chain_test
    chrono_keeper_extraction_chain_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/DualEndpointChunkExtractorRDMA.cpp
)

target_include_directories(keeper_extraction_chain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_extraction_chain_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(keeper_extraction_chain_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-extraction-chain)
gtest_discover_tests(keeper_extraction_chain_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_extraction_chain_test DESTINATION tests)
endif()

# Release-before-decay data-loss reproduction. KeeperDataStore/KeeperStoryPipeline
# are only compiled into the chrono_keeper executable (no shared lib), so pull the
# two .cpp files directly into the test target.
add_executable(keeper_release_dataloss_test
    chrono_keeper_release_dataloss_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/KeeperStoryPipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/KeeperDataStore.cpp
)

target_include_directories(keeper_release_dataloss_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_release_dataloss_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(keeper_release_dataloss_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-release-dataloss)
gtest_discover_tests(keeper_release_dataloss_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_release_dataloss_test DESTINATION tests)
endif()

# Story retirement releases the story's tail so its durable chunks can be freed.
# Same in-process KeeperDataStore setup as the data-loss test above.
add_executable(keeper_story_retirement_test
    chrono_keeper_story_retirement_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/KeeperStoryPipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/KeeperDataStore.cpp
)

target_include_directories(keeper_story_retirement_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_story_retirement_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(keeper_story_retirement_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-story-retirement)
gtest_discover_tests(keeper_story_retirement_test TEST_PREFIX "Unit_ChronoKeeper_" PROPERTIES TIMEOUT 60)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_story_retirement_test DESTINATION tests)
endif()

# WAL replay (GTest) — events read back from the WAL at startup become sealed
# chunks in the retention store. Same in-process KeeperDataStore setup as the
# data-loss test above.
add_executable(keeper_wal_replay_test
    chrono_keeper_wal_replay_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/KeeperStoryPipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/KeeperDataStore.cpp
)

target_include_directories(keeper_wal_replay_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_wal_replay_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(keeper_wal_replay_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-wal-replay)
gtest_discover_tests(keeper_wal_replay_test TEST_PREFIX "Unit_ChronoKeeper_" PROPERTIES TIMEOUT 60)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_wal_replay_test DESTINATION tests)
endif()

# Orphan recovery (GTest) — IngestionQueue orphan helpers + seal-to-extraction flow
add_executable(keeper_orphan_recovery_test
    chrono_keeper_orphan_recovery_test.cpp
)

target_include_directories(keeper_orphan_recovery_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_orphan_recovery_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(keeper_orphan_recovery_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-orphan-recovery)
gtest_discover_tests(keeper_orphan_recovery_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_orphan_recovery_test DESTINATION tests)
endif()

# WalFileSink (GTest) — the POSIX file layer under the keeper WAL and the
# fault-injecting test sink, in a temporary directory. Thallium for the test
# that holds a sync on an Argobots stream.
add_executable(keeper_wal_file_sink_test
    chrono_keeper_wal_file_sink_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalFileSink.cpp
)

target_include_directories(keeper_wal_file_sink_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
)

target_link_libraries(keeper_wal_file_sink_test PRIVATE
    GTest::gtest_main
    thallium
)

set_target_properties(keeper_wal_file_sink_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-wal-file-sink)
gtest_discover_tests(keeper_wal_file_sink_test TEST_PREFIX "Unit_ChronoKeeper_" PROPERTIES TIMEOUT 60)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_wal_file_sink_test DESTINATION tests)
endif()

# WalReleaseTracker (GTest) — the WAL's table of pending durable events per
# story and per segment, which decides when a segment can be deleted.
add_executable(keeper_wal_release_tracker_test
    chrono_keeper_wal_release_tracker_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalReleaseTracker.cpp
)

target_include_directories(keeper_wal_release_tracker_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_wal_release_tracker_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(keeper_wal_release_tracker_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-wal-release-tracker)
gtest_discover_tests(keeper_wal_release_tracker_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_wal_release_tracker_test DESTINATION tests)
endif()

# WalFormat (GTest) — the keeper WAL's record frames and the segment scan
# recovery runs, on byte buffers and one temporary file.
add_executable(keeper_wal_format_test
    chrono_keeper_wal_format_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalFormat.cpp
)

target_include_directories(keeper_wal_format_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_wal_format_test PRIVATE
    GTest::gtest_main
    chrono_common
)

set_target_properties(keeper_wal_format_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-wal-format)
gtest_discover_tests(keeper_wal_format_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_wal_format_test DESTINATION tests)
endif()

# WalConf (GTest) — the keeper's "WAL" block: defaults, the shipped template's
# block and the validation. ChronoKeeperConfiguration.cpp is only compiled into
# the chrono_keeper executable, so the test pulls it in directly.
add_executable(keeper_wal_conf_test
    chrono_keeper_wal_conf_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/ChronoKeeperConfiguration.cpp
)

target_include_directories(keeper_wal_conf_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_compile_definitions(keeper_wal_conf_test PRIVATE
    CHRONOLOG_CONF_TEMPLATE="${CMAKE_SOURCE_DIR}/conf/default_conf.json.in"
)

target_link_libraries(keeper_wal_conf_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
)

set_target_properties(keeper_wal_conf_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-wal-conf)
gtest_discover_tests(keeper_wal_conf_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_wal_conf_test DESTINATION tests)
endif()

# WalMountCheck (GTest) — the warnings the keeper logs at startup about the WAL
# directory's file system, device and free space, plus one real inspection.
add_executable(keeper_wal_mount_check_test
    chrono_keeper_wal_mount_check_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalMountCheck.cpp
)

target_include_directories(keeper_wal_mount_check_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
)

target_link_libraries(keeper_wal_mount_check_test PRIVATE
    GTest::gtest_main
)

set_target_properties(keeper_wal_mount_check_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-wal-mount-check)
gtest_discover_tests(keeper_wal_mount_check_test TEST_PREFIX "Unit_ChronoKeeper_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_wal_mount_check_test DESTINATION tests)
endif()

# WalSegmentManager (GTest) — the keeper WAL directory's lock, partial-file
# cleanup, pre-zeroed spares with their H record, and segment deletion, in a
# temporary directory. Thallium for the preparer's Argobots stream.
add_executable(keeper_wal_segment_manager_test
    chrono_keeper_wal_segment_manager_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalSegmentManager.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalFileSink.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalFormat.cpp
)

target_include_directories(keeper_wal_segment_manager_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_wal_segment_manager_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(keeper_wal_segment_manager_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-wal-segment-manager)
gtest_discover_tests(keeper_wal_segment_manager_test TEST_PREFIX "Unit_ChronoKeeper_" PROPERTIES TIMEOUT 60)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_wal_segment_manager_test DESTINATION tests)
endif()

# WalRecovery (GTest) — the keeper WAL read back at startup: events, names and
# copies in log order, D records, the torn tail of the newest segment, and the
# refusals that leave every segment unchanged, in a temporary directory.
# Thallium for the segment manager's Argobots mutex; the release tracker for
# the test that hands the recovered copies to it.
add_executable(keeper_wal_recovery_test
    chrono_keeper_wal_recovery_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalRecovery.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalSegmentManager.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalFileSink.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalFormat.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalReleaseTracker.cpp
)

target_include_directories(keeper_wal_recovery_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_wal_recovery_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(keeper_wal_recovery_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-wal-recovery)
gtest_discover_tests(keeper_wal_recovery_test TEST_PREFIX "Unit_ChronoKeeper_" PROPERTIES TIMEOUT 60)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_wal_recovery_test DESTINATION tests)
endif()

# KeeperWal (GTest) — the keeper WAL's committer: group commit on its own
# Argobots stream, the answer only after the sync, the failure latch, the
# free-space reserve, N records per segment, oversized records, retries that
# join a write in flight, releases and D records, and stop. Runs in a temporary
# directory through the fault-injecting sink, with requests from ULTs.
add_executable(keeper_wal_test
    chrono_keeper_wal_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/KeeperWal.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalSegmentManager.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalReleaseTracker.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalFileSink.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalFormat.cpp
)

target_include_directories(keeper_wal_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(keeper_wal_test PRIVATE
    GTest::gtest_main
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(keeper_wal_test PROPERTIES OUTPUT_NAME chronolog-test-keeper-wal)
# a waiter that is never answered shows as a hang
gtest_discover_tests(keeper_wal_test TEST_PREFIX "Unit_ChronoKeeper_" PROPERTIES TIMEOUT 120)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(keeper_wal_test DESTINATION tests)
endif()
