---
sidebar_position: 5
title: "Testing"
---

# Testing

ChronoLog's test suite covers four broad categories: **unit**, **integration**, **communication/overhead**, and **system/workload** tests. The full suite is registered with CMake's CTest framework except for a handful of Python system scripts and shell workload drivers that live outside the CTest tree.

A debug build exposes the largest set of tests (~133 CTest entries). Some entries are **DISABLED** in the default run; others are marked **MANUAL** because they require a live multi-process ChronoLog deployment before they can succeed.

---

## How to Run Tests

### CTest (automated)

From your CMake build directory:

```bash
# List all registered tests without running them
ctest -N

# Run all enabled tests (parallel, verbose)
ctest --output-on-failure -j$(nproc)

# Run a specific subset by name regex
ctest -R "Unit_ChronoCommon_StoryChunk" --output-on-failure

# Run including DISABLED tests
ctest -R "." --output-on-failure
```

:::info
`ctest -N` is the authoritative source of truth for test counts and names in a given build. Debug builds include extra GTest cases (e.g. TRACE/DEBUG macro tests) that are absent from release builds.
:::

### Manual Tests

Tests labelled **MANUAL** require ChronoVisor, ChronoKeeper, and the client library to be running before execution. Start the deployment first (see the [Docker tutorials](../../tutorials/docker-single-node.md)), then invoke the binary directly:

```bash
# Example — client connectivity test
./test/integration/client/client_connect_rpc_test

# Shell-based communication protocol test
bash test/communication/thallium_protocol_test.sh
```

### Disabled Tests

The following tests are registered with CTest but excluded from the default run. They can be forced with `ctest -R <name>`:

| CTest Name | Source File | Reason |
|---|---|---|
| `Unit_ChronoCommon_ExtractionChain` | `test/unit/chrono-common/chrono_common_extraction_chain_test.cpp` | Under active development |
| `Unit_ChronoCommon_ChunkConsumerService` | `test/unit/chrono-common/chrono_common_chunk_consumer_service_test.cpp` | Under active development |
| `Unit_ChronoPlayer_StoryChunkTransferAgent` | `test/unit/chrono-player/chrono_player_transfer_agent_test.cpp` | Under active development |
| `Unit_ChronoPlayer_PlaybackServiceLifecycle` | `test/unit/chrono-player/chrono_player_playback_service_test.cpp` | Under active development |

---

## Server Tests

:::info
The three GTest suites below (`story_chunk_test`, `story_pipeline_test`, `chronokvs_logger_test`) are discovered at runtime by GTest. CTest registers each individual test case as a separate entry, so a single `ctest -R Unit_ChronoCommon_StoryChunk` run executes all 47 cases.
:::

### Unit Tests – StoryChunk

47 GTest cases. CTest name prefix: `Unit_ChronoCommon_StoryChunk_`

**TestConstructors**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testEmptyConstructor` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Empty StoryChunk has no events |
| `testNormalCtorInit` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Start/end times set correctly |
| `testBoundaryCtorInit` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | `endTime < startTime` is auto-corrected to `startTime + 5000` |

**TestInsertEvent**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testNormalInsertEvent` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Single event inserted and retrieved |
| `testInsertAtEndTime` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Event at `endTime` boundary rejected |
| `testInsertBeforeStart` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Event before `startTime` rejected |
| `testInsertAtStartTime` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Event at `startTime` accepted |
| `testOutOfOrderInsert` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | 6 out-of-order events stored chronologically |
| `testDuplicateKeyCombinations` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Same timestamp with different client/index accepted; true duplicate rejected |
| `testStressInsert` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | 100 000 events inserted and verified |
| `testIncorrectEventId` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Event with wrong `storyId` rejected |
| `testInsertLargeEventRecord` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | 20 KB payload event accepted |

**TestMergeEvents**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testEmptyInputMap` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Merge of empty map does nothing |
| `testStartBeforeWindow` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Only in-window events merged from map |
| `testValidMerge` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | 2 valid events merged and map emptied |
| `testAllInvalidMerges` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | All out-of-window events rejected |
| `testDuplicateTimestamps` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | 3 events at same timestamp all merged |
| `testHugeMerge` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | 1 000 events merged at once |
| `BoundaryEndTimeExclusion` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Event at `endTime` excluded; `endTime-1` included |
| `testMergeFromMiddle` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Merge starts from arbitrary iterator position |
| `testMergeEmptyChunk` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Merging empty StoryChunk changes nothing |
| `testMergeWithinWindow` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | 2 in-window events merged from other chunk |
| `testMergeOutsideWindow` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Only in-window subset merged |
| `testMixedChunkMerge` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Overlap events from differently-windowed chunk |
| `testMergeStartTimeIncorrect` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | `mergeStart` before/after window still bounded by chunk window |
| `testMergeMapThenChunk` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Sequential map-merge then chunk-merge both succeed |
| `testIncorrectStoryId` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Wrong `storyId` event rejected |
| `testEmptyAndHugePayloads` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Empty-record and 5 KB payload handled |
| `testMergeStartBeforeEndInside` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Other chunk starts before, ends inside |
| `testMergeStartInsideEndAfter` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Other chunk starts inside, ends after |
| `testMergeExactBoundaryNoOverlap` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | No overlap at exact boundary |

**TestEraseEvents**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testValidErase` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Erase single event by timestamp range |
| `testNonExistingTimestamp` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Erase non-existent timestamp is no-op |
| `testEraseDuplicateTimestamps` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Both events at same timestamp erased |
| `testEraseInRange` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Range erase removes all events in `[120, 160)` |
| `testEraseOutOfTime` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Range outside chunk window is no-op |
| `testEraseInvalidTime` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | `start > end` is no-op |
| `testEraseAllEvents` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Erase `[10, 2000)` removes all events |
| `testStressErase` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Erase 20 000 events from a 100 000-event chunk |
| `testEraseEndTimeMinusOne` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | `endTime-1` erased; others kept |
| `testZeroRange` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Erase `[x, x)` is no-op |

**TestExtractEventSeries**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testExtractEmptyChunk` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Extract from empty chunk returns empty vector |
| `testExtractSingleEvent` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Single event extracted with correct fields |
| `testExtractEventsSorted` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | 7 events extracted in ascending order |
| `testExtractTwice` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Second extract returns empty after chunk cleared |
| `testExtractAfterReinsert` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | Insert → extract → insert → extract cycle |
| `testExtractLargePayload` | Unit | GTest | `test/unit/chrono-common/story_chunk_test.cpp` | 5 KB payload preserved after extract |

---

### Unit Tests – StoryPipeline

28 GTest cases. CTest name prefix: `Unit_ChronoCommon_StoryPipeline_`

**TestConstructors**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testValidEmptyInit` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Pipeline created with zero `startTime` |
| `testOnBoundaryStartTime` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Granularity-aligned start kept; 3 chunks allocated |
| `testNonBoundaryRounding` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Non-aligned start rounded to previous boundary |
| `testHugeStoryStartTimeNoOverflow` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Near-max `startTime` with no `uint64` overflow |

**TestGetActiveIngestionHandle**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testNonNullHandleEmptyDeques` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Handle non-null; active and passive deques empty |

**TestCollectIngestedEvents**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testNoChunkValid` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | No chunk → collect is no-op |
| `testSingleEmptyChunk` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Empty chunk removed; deques stay empty |
| `testSingleNonEmptyChunk` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Non-empty chunk merged and deleted |
| `testNullptrInActiveDeque` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | `nullptr` entry skipped without crash |

**TestExtractDecayedChunks**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testEmptyExtract` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Extract before decay threshold extracts nothing |
| `testExtractSmallAfterDecay` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Non-empty chunk stashed after decay |
| `testExtractMultiple` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Two chunks stashed past both decay points |
| `testNoAppendAfterSingleDecay` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Timeline shrinks; no append when ≥2 chunks remain |
| `testAppendBehaviorAfterMultiDecay` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | New chunk appended when only 1 chunk left |
| `testExtractLeavesTwo` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Empty head chunks removed; future non-empty chunk preserved |

**MergeEvents**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testEmptyMerge` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Merging empty chunk is no-op |
| `testPrependSuccess` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Event before `TimelineStart` causes prepend |
| `testSingleAppend` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Event after `TimelineEnd` extends timeline by 1 chunk |
| `testMultipleAppend` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Far-future event extends timeline multiple chunks |

**TestPrependStoryChunk**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testSuccess` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Prepend moves `TimelineStart` back by 1 granularity |

**TestAppendStoryChunk**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testSuccess` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Append moves `TimelineEnd` forward by 1 granularity |

**TestFinalize**

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `testNoPendingChunks` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Finalize on empty pipeline does nothing |
| `testOnlyPassiveDeque` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Passive-deque chunk stashed |
| `testOnlyActiveDeque` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Active-deque chunk stashed |
| `testMixedDeques` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Passive then active chunks stashed in FIFO order |
| `testEmptyVsNonTimeline` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Empty head chunks skipped; non-empty stashed |
| `testFinalizeDoubleCall` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | Second finalize is a safe no-op |
| `testFinalizeWithMixedTimeline` | Unit | GTest | `test/unit/chrono-common/story_pipeline_test.cpp` | 3 non-empty chunks in pipeline all stashed |

---

### Unit Tests – ChronoKVS Logger

25 GTest cases in a debug build (22 in release). CTest name prefix: `Unit_ChronoKVS_ChronoKVSLogger_`

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `LogLevel_EnumOrdering` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | Log-level enum ordering is correct |
| `LogLevel_ExactValues` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | Log-level enum exact integer values |
| `DefaultLevel_ReturnsExpected` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | `getDefaultLogLevel()` returns DEBUG (debug build) or ERROR (release) |
| `LogLevelToString_AllLevels` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | All 7 levels convert to correct string |
| `LogLevelToString_Unknown` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | Unknown level returns fallback string |
| `FormatMessage_SingleString` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | Format with a single string argument |
| `FormatMessage_OneIntArg` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | Format with one integer argument |
| `FormatMessage_StringArg` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | Format with one string argument |
| `FormatMessage_MultipleArgs` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | Format with multiple mixed arguments |
| `Macros_InfoEnabled` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | INFO macro fires when level ≤ INFO |
| `Macros_InfoDisabled` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | INFO macro suppressed when level > INFO |
| `Macros_WarningEnabled` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | WARNING macro fires when level ≤ WARNING |
| `Macros_WarningDisabled` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | WARNING macro suppressed when level > WARNING |
| `Macros_ErrorEnabled` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | ERROR macro fires when level ≤ ERROR |
| `Macros_ErrorDisabled` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | ERROR macro suppressed when level > ERROR |
| `Macros_CriticalEnabled` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | CRITICAL macro fires when level ≤ CRITICAL |
| `Macros_CriticalDisabled` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | CRITICAL macro suppressed when level > CRITICAL |
| `Macros_OffDisablesAll` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | OFF level suppresses all macros |
| `DebugMacros_TraceEnabled` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | TRACE macro fires at TRACE level *(debug build only)* |
| `DebugMacros_TraceDisabled` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | TRACE macro suppressed above TRACE level *(debug build only)* |
| `DebugMacros_DebugEnabled` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | DEBUG macro fires at DEBUG level *(debug build only)* |
| `DebugMacros_DebugDisabled` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | DEBUG macro suppressed above DEBUG level *(debug build only)* |
| `DebugMacros_AllLevelsWithTrace` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | All levels produce output with TRACE active *(debug build only)* |
| `ReleaseMacros_TraceDebugAreNoops` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | TRACE/DEBUG are no-ops in release builds *(release build only)* |
| `ReleaseMacros_InfoStillWorks` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | INFO still works after TRACE/DEBUG no-op *(release build only)* |
| `LogMessage_DirectCall` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | Direct `log_message()` call at each level |
| `LogMessage_AllLevelsDirect` | Unit | GTest | `test/unit/chronokvs/chronokvs_logger_test.cpp` | All levels exercised via `log_message()` |

---

### Unit Tests – ChronoStore

2 standalone CTest entries.

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `Unit_ChronoStore_HDF5ArchiverWriteReadRoundtrip` | Unit | Custom | `test/unit/chrono-store/chrono_store_hdf5_archiver_test.cpp` | Write events to HDF5 archive and read back with matching content |
| `Unit_ChronoStore_VlenBytesVsBlobMapComparison` | Unit | Custom | `test/unit/chrono-store/chrono_store_vlen_bytes_vs_blob_map_test.cpp` | Compare variable-length bytes and blob-map HDF5 storage strategies |

---

### Unit Tests – ChronoPlayer

4 standalone CTest entries; 2 are disabled in the default run.

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `Unit_ChronoPlayer_HDF5ArchiveReadingAgent` | Unit | Custom | `test/unit/chrono-player/chrono_player_hdf5_archive_reader_test.cpp` | HDF5 reading agent retrieves story chunks from archive |
| `Unit_ChronoPlayer_FileSystemMonitoring` | Unit | Custom | `test/unit/chrono-player/chrono_player_fs_monitoring_test.cpp` | File-system watcher detects new archive files |
| `Unit_ChronoPlayer_StoryChunkTransferAgent` *(DISABLED)* | Unit | Custom | `test/unit/chrono-player/chrono_player_transfer_agent_test.cpp` | Transfer agent lifecycle (under development) |
| `Unit_ChronoPlayer_PlaybackServiceLifecycle` *(DISABLED)* | Unit | Custom | `test/unit/chrono-player/chrono_player_playback_service_test.cpp` | Playback service start/stop lifecycle (under development) |

---

### Integration Tests – Keeper-Grapher

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `Integration_KeeperGrapher_StoryChunkIngest` | Integration | Custom | `test/integration/keeper-grapher/keeper_grapher_ingest_test.cpp` | ChronoKeeper ingest pipeline stores chunks in ChronoGrapher |
| `Integration_KeeperGrapher_StoryChunkExtract` | Integration | Custom | `test/integration/keeper-grapher/keeper_grapher_extract_test.cpp` | Decay-triggered extraction delivers chunks from Keeper to Grapher |

---

### Integration Tests – Package Discovery

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `PackageDiscovery_CMakeFindPackage` | Integration | CMake | `test/integration/package-discovery/package_discovery_cmake_test.cpp` | `find_package(ChronoLog)` locates headers and libraries in-tree |
| `PackageDiscovery_PkgConfig` | Integration | CMake | `test/integration/package-discovery/package_discovery_pkgconfig_test.cpp` | `pkg-config --cflags chronolog` returns correct flags in-tree |
| `PackageDiscovery_Installed_CMakeFindPackage` | Integration | CMake script | *(CMake script)* | `find_package(ChronoLog)` succeeds after `cmake --install` |
| `PackageDiscovery_Installed_PkgConfig` | Integration | CMake script | *(CMake script)* | `pkg-config` succeeds after `cmake --install` |

---

### Communication Tests

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `Communication_Thallium_ServerRepeaterRpc` | Communication | Custom | `test/communication/thallium_server_test.cpp` | Thallium RPC server accepts and echoes RPCs |
| `Communication_Thallium_ClientMpi` | Communication | MPI | `test/communication/thallium_client_mpi_test.cpp` | MPI-launched client exchanges RPCs with server |
| `Communication_Thallium_ClientSendRecvOrRdma` *(MANUAL)* | Communication | Custom | `test/communication/thallium_client_test.cpp` | Send/recv and RDMA paths tested with live server |
| `Communication_Thallium_ProtocolScript` *(MANUAL)* | Communication | Shell | `test/communication/thallium_protocol_test.sh` | Shell script probes all supported Thallium transports |

---

### Overhead / Benchmark Tests

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `Overhead_Clock_HighResolutionClock` | Overhead | Custom | `test/overhead/clock/clock_high_resolution_test.cpp` | `clock_gettime` resolution and monotonicity |
| `Overhead_Clock_TimestampCollection` | Overhead | Custom | `test/overhead/clock/clock_timestamp_collection_test.cpp` | Throughput of timestamp-collection in a tight loop |
| `Overhead_Clock_TimestampCollisionDetection` | Overhead | Custom | `test/overhead/clock/clock_timestamp_collision_test.cpp` | Collision-detection logic under concurrent timestamp pressure |
| `Overhead_Clock_Script` *(MANUAL)* | Overhead | Shell | `test/overhead/clock/clock_cset_shield_test.sh` | CPU-set shielding effect on clock overhead |
| `Overhead_Lock_ContentionBenchmark` | Overhead | Custom | `test/overhead/lock/lock_overhead_test.cpp` | Mutex vs. spinlock contention benchmark |

---

### System & Workload Scripts

These scripts are **not registered with CTest**. Run them directly after a full deployment.

| Script | Type | Description |
|---|---|---|
| `test/system/fidelity_test_01.py` | System | Python fidelity test — scenario 01 |
| `test/system/fidelity_test_02.py` | System | Python fidelity test — scenario 02 |
| `test/system/fidelity_test_03.py` | System | Python fidelity test — scenario 03 |
| `test/system/fidelity_test_all.py` | System | Drives all three fidelity scenarios in sequence |
| `test/synthetic_workload/perf_test.sh` | Workload | MPI-launched performance benchmark |
| `test/synthetic_workload/distributed_syslog.sh` | Workload | Distributed syslog simulation across multiple nodes |

---

## Client Tests

:::warning
All client integration tests require a **live ChronoLog deployment** (ChronoVisor + ChronoKeeper + ChronoGrapher) before they will succeed. Start the system first — see the [Docker tutorials](../../tutorials/docker-single-node.md) — then run these tests manually.
:::

### Integration Tests

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `Integration_Client_ConnectRpc` | Integration | Custom | `test/integration/client/client_connect_rpc_test.cpp` | Client connects to ChronoVisor via RPC |
| `Integration_Client_MetadataRpc` | Integration | Custom | `test/integration/client/client_metadata_rpc_test.cpp` | Create/destroy chronicle and story metadata RPCs |
| `Integration_Client_MultiPthread` | Integration | Custom | `test/integration/client/client_multi_pthread_test.cpp` | Multiple POSIX threads sharing a single client handle |
| `Integration_Client_ThreadInterdependency` | Integration | Custom | `test/integration/client/client_thread_interdependency_test.cpp` | Threads depend on each other's story events |
| `Integration_Client_MultiStorytellers` | Integration | Custom | `test/integration/client/client_multi_storytellers_test.cpp` | Multiple storyteller handles writing concurrently |
| `Integration_Client_StoryReader` | Integration | Custom | `test/integration/client/client_story_reader_test.cpp` | Reader API retrieves events written by a storyteller |
| `Integration_Client_MultiArgobots` | Integration | Custom | `test/integration/client/client_multi_argobots_test.cpp` | Multiple Argobots ULTs sharing a client |
| `Integration_Client_MultiOpenmp` | Integration | Custom | `test/integration/client/client_multi_openmp_test.cpp` | OpenMP threads writing events in parallel |
| `Integration_Client_HybridArgobots` | Integration | Custom | `test/integration/client/client_hybrid_argobots_test.cpp` | Hybrid Argobots + POSIX thread concurrency |

---

### Communication Tests – Client

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `Communication_Thallium_ClientSendRecvOrRdma` *(MANUAL)* | Communication | Custom | `test/communication/thallium_client_test.cpp` | Client-side send/recv and RDMA paths with live server |

---

### ChronoKVS Integration

| Test Name | Type | Framework | Source File | Description |
|---|---|---|---|---|
| `Integration_ChronoKVS_PluginIntegration` *(MANUAL)* | Integration | Custom | `test/integration/chronokvs/chronokvs_integration_test.cpp` | ChronoKVS plugin end-to-end write/read with live deployment |

:::info
`Integration_ChronoKVS_PluginIntegration` has a **300-second CTest timeout**. It exercises the full ChronoKVS write and read path, so a running ChronoLog system and a populated KVS store are required.
:::
