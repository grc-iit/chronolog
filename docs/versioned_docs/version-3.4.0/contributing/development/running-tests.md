---
sidebar_position: 4
title: "Running Tests"
---

# Running Tests

ChronoLog's tests come in three groups:

- **CTest entries** in the build tree: unit tests that need nothing running, and integration, end-to-end, communication and overhead tests that need a running deployment.
- **Integration scripts** in `tests/integration/*.sh` that deploy their own daemons, run a scenario and stop them. They are not registered with CTest.
- **The distributed pipeline**, a 7-container deployment that CI runs on every pull request and that `docker/local_distributed_ci.sh` runs locally. See [CI/CD](../guidelines/ci-cd.md).

A Debug build registers 527 CTest entries: 480 unit tests, 6 functional tests (5 disabled), 16 integration tests, 12 end-to-end tests and their fixtures, 4 package discovery tests, 4 communication tests and 5 overhead tests. `ctest -N` lists them for your build; a Release build has fewer, because some logger tests exist only in Debug.

---

## How to Run Tests

### Unit tests

Build through the deploy script, then run CTest in the build directory. The script builds into `<build dir>/<build type>`, so the path ends in `Debug`:

```bash
tools/deploy/local_single_user_deploy.sh -b -t Debug
tools/deploy/local_single_user_deploy.sh -i -t Debug

ctest --test-dir ~/chronolog-build/Debug -R 'Unit_' --output-on-failure -j4
```

The unit tests need no running daemons. A few bind a fixed loopback port (`Unit_ChronoCommon_ExtractionChainConfig` uses 2225), so stop a local deployment or another test run that holds it. Some binaries get their library paths only at install, so install before running CTest.

Other useful forms:

```bash
ctest --test-dir ~/chronolog-build/Debug -N                       # list without running
ctest --test-dir ~/chronolog-build/Debug -R 'Unit_ChronoPlayer_'  # one component
ctest --test-dir ~/chronolog-build/Debug -R 'ArchiveManifest'     # any name matching a regex
```

GTest suites are registered case by case, so `-R Unit_ChronoCommon_StoryChunk` runs all 47 StoryChunk cases.

### Tests that need a running deployment

The `Integration_`, `EndToEnd_`, `Communication_` and `Overhead_` entries connect to a running ChronoLog. Start one first, then run them:

```bash
~/chronolog-install/chronolog/tools/deploy/deploy_local.sh -d -w ~/chronolog-install/chronolog -k 2 -r 1
ctest --test-dir ~/chronolog-build/Debug -R 'Integration_Client_|EndToEnd_' --output-on-failure
~/chronolog-install/chronolog/tools/deploy/deploy_local.sh -s -w ~/chronolog-install/chronolog
```

The local pipeline in CI runs the whole CTest set against a single-host deployment of 5 keepers and 3 recording groups.

### Disabled tests

These are registered but excluded from the default run. Force one with `ctest -R <name>`:

| CTest Name | Source File |
|---|---|
| `Functional_ChronoKeeper_ExtractionChain` | `tests/functional/chrono-keeper/chrono_keeper_extraction_chain_test.cpp` |
| `Functional_ChronoGrapher_ExtractionChain` | `tests/functional/chrono-grapher/chrono_grapher_extraction_chain_test.cpp` |
| `Functional_ChronoPlayer_ChunkConsumer` | `tests/functional/chrono-player/story_chunk_consumer_service_test.cpp` |
| `Functional_ChronoPlayer_QueryResponseAgent` | `tests/functional/chrono-player/chrono_player_transfer_agent_test.cpp` |
| `Functional_ChronoPlayer_PlaybackServiceLifecycle` | `tests/functional/chrono-player/chrono_player_playback_service_test.cpp` |

---

## Unit Tests

One row per test binary. **Cases** is the number of CTest entries it registers.

### Common (`tests/unit/chrono-common/`)

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronoCommon_StoryChunk_` | 47 | `chrono_common_story_chunk_test.cpp` | Inserting, merging, erasing and extracting events in a StoryChunk, including window boundaries and duplicates |
| `Unit_ChronoCommon_StoryPipeline_` | 37 | `chrono_common_story_pipeline_test.cpp` | The story pipeline's timeline: chunk allocation, ingestion, decay, prepend/append, finalize, receipts and incarnation numbers |
| `Unit_ChronoCommon_ArchiveManifest` | 27 | `chrono_common_archive_manifest_test.cpp` | Manifest records, the writer's appends and the reader's tail, including a log replaced on the same inode |
| `Unit_ChronoCommon_ChunkWriter`, `Unit_ChronoCommon_ArchiveLayout` | 14 | `chrono_common_story_chunk_writer_test.cpp` | Writing a window through a temporary name into its story directory; name encoding and file names |
| `Unit_ChronoCommon_ConfTemplate` | 6 | `chrono_common_conf_template_test.cpp` | The shipped configuration template keeps the timing rules between components |
| `Unit_ChronoCommon_DataStoreConf` | 6 | `chrono_common_data_store_conf_test.cpp` | Validation of the DataStoreInternals settings |
| `Unit_ChronoCommon_ExtractionModule` | 5 | `chrono_common_extraction_module_test.cpp` | The drain loop shared by the keeper and the grapher |
| `Unit_ChronoCommon_ChunkExtractorCSV` | 2 | `chrono_common_chunk_extractor_csv_test.cpp` | The CSV extractor and empty windows |
| `Unit_ChronoCommon_ExtractionChainConfig` | 1 | `chrono_common_extraction_chain_config_test.cpp` | Parsing extraction chain configurations (custom `main`, not GTest) |

### ChronoKeeper (`tests/unit/chrono-keeper/`)

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronoKeeper_KeeperChunkRetentionStore` | 57 | `chrono_keeper_retention_store_test.cpp` | The keeper's retention store: the tail index, retained chunks, and freeing a chunk only once the grapher has written it |
| `Unit_ChronoKeeper_KeeperReleaseDataLoss`, `..._KeeperOrphanAtRetirementDataLoss` | 5 | `chrono_keeper_release_dataloss_test.cpp` | Releasing a story does not lose events still in its pipeline |
| `Unit_ChronoKeeper_KeeperExtractionChain` | 4 | `chrono_keeper_extraction_chain_test.cpp` | The chain hands extracted chunks to the retention store instead of deleting them |
| `Unit_ChronoKeeper_KeeperOrphanRecovery` | 3 | `chrono_keeper_orphan_recovery_test.cpp` | Events that arrive for a story with no pipeline are sealed and kept |
| `Unit_ChronoKeeper_KeeperChunkRetentionCap` | 2 | `chrono_keeper_retention_cap_test.cpp` | The `retention_cap_mb` alarm |
| `Unit_ChronoKeeper_KeeperStoryRetirement` | 2 | `chrono_keeper_story_retirement_test.cpp` | A retired story frees its chunks only when they are written and out of the tail |

### ChronoGrapher (`tests/unit/chrono-grapher/`)

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronoGrapher_StoryWatermarkRegistry` | 31 | `story_watermark_registry_test.cpp` | The per-story persisted watermark W and the receipts of delivered keeper chunks |
| `Unit_ChronoGrapher_HDF5FileChunkExtractorWatermark` | 26 | `hdf5_file_chunk_extractor_test.cpp` | The HDF5 extractor advances W and settles receipts only after a window and its manifest record are written |
| `Unit_ChronoGrapher_GrapherConfiguration` | 8 | `grapher_configuration_test.cpp` | A grapher refuses a chain without the HDF5 extractor |
| `Unit_ChronoGrapher_WatermarkReportTransport` | 7 | `watermark_report_transport_test.cpp` | Watermark reports from the grapher to the keepers over real engines on loopback |
| `Unit_ChronoGrapher_GrapherDestroy`, `..._ExtractionQueue`, `..._GrapherExtractionChain` | 5 | `grapher_destroy_test.cpp` | A destroy waits for windows still being written |
| `Unit_ChronoGrapher_GrapherDataCollectionReport` | 2 | `grapher_data_collection_report_test.cpp` | Watermark reports sent from the data-collection loop |
| `Unit_ChronoGrapher_GrapherPipelineRetirement` | 2 | `grapher_pipeline_retirement_test.cpp` | A stopped story's pipeline writes its last windows before it is retired |

### ChronoPlayer (`tests/unit/chrono-player/`)

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronoPlayer_ArchiveManifestIndex` | 22 | `chrono_player_archive_manifest_index_test.cpp` | The player finds archive files only through the manifest, and drops the files of destroyed stories |
| `Unit_ChronoPlayer_HotRangeSplit`, `Unit_ChronoPlayer_ReplayPlan` | 13 | `chrono_player_hot_range_split_test.cpp` | Where a replay splits between the archive and the keepers |
| `Unit_ChronoPlayer_ArchiveNumberedFiles` | 5 | `chrono_player_archive_numbered_files_test.cpp` | A window written more than once is read from every file |
| `Unit_ChronoPlayer_KeeperHotFetch` | 4 | `chrono_player_keeper_hot_fetch_client_test.cpp` | Fetches from all keepers at once; a silent keeper costs one deadline |
| `Unit_ChronoPlayer_ReplayEventMerge` | 4 | `chrono_player_replay_event_merge_test.cpp` | Merging archive and keeper events without duplicates |
| `Unit_ChronoPlayer_ArchiveClientId` | 2 | `chrono_player_archive_client_id_test.cpp` | The full 64-bit client id survives the archive |
| `Unit_ChronoPlayer_ArchiveVisibility` | 2 | `chrono_player_archive_visibility_test.cpp` | Archive reads at the split point |
| `Unit_ChronoPlayer_PlayerReaderConf` | 2 | `chrono_player_reader_conf_test.cpp` | The `ArchiveReaders` block; removed settings are ignored |
| `Unit_ChronoPlayer_PlayerPipelineRetirement` | 2 | `player_pipeline_retirement_test.cpp` | A stopped story still answers queries until it is retired |
| `Unit_ChronoPlayer_HDF5ArchiveReadingAgent` | 1 | `chrono_player_hdf5_archive_reader_test.cpp` | Reading story chunks from HDF5 files (custom `main`, not GTest) |

### ChronoVisor (`tests/unit/chrono-visor/`)

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronoVisor_StoryGroupSelection`, `..._ActiveGroups`, `..._ActiveRecordingGroup` | 12 | `story_group_selection_test.cpp` | A story's recording group comes from its story id, stays the same when it is acquired again, and moves only when the groups change |
| `Unit_ChronoVisor_ChronicleStoryId` | 2 | `chronicle_story_id_test.cpp` | Name pairs that concatenate alike get different story ids |

### Client library (`tests/unit/chronolog-client/`)

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronologClient_QueryServiceAddressTest` | 10 | `query_service_address_test.cpp` | The address a reader listens on when the visor is on another host |
| `Unit_ChronologClient_ClientIdentityTest` | 5 | `chronolog_client_identity_test.cpp` | The `ClientId` layout |
| `Unit_ChronologClient_ClientLoginName` | 4 | `client_login_name_test.cpp` | The logged user name when the process has no login name |

### Plugins

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronoSQL_ChronoSQLParser`, `..._RowCodec`, `..._Metadata` | 28 | `tests/unit/chronosql/chronosql_*_test.cpp` | ChronoSQL's parser, row encoding and table metadata |
| `Unit_ChronoKVS_ChronoKVSLogger_` | 25 | `tests/unit/chronokvs/chronokvs_logger_test.cpp` | ChronoKVS logging levels and macros (Debug build; Release registers fewer) |
| `Unit_ChronoPubSub_ChronoPubSubLogger_` | 25 | `tests/unit/chronopubsub/chronopubsub_logger_test.cpp` | The same for ChronoPubSub |
| `Unit_ChronoKVS_ChronoKVSConfig`, `Unit_ChronoPubSub_ChronoPubSubConfig`, `Unit_ChronoSQL_ChronoSQLConfig` | 6 | `tests/unit/chrono{kvs,pubsub,sql}/*_config_test.cpp` | Loading each plugin's configuration file |

### Deploy and CI scripts (`tests/unit/*.sh`)

Shell tests that check scripts without deploying anything. Each is one CTest entry.

| CTest Name | Source File | What it covers |
|---|---|---|
| `Unit_Deploy_ArchiveCleanup` | `deploy_archive_cleanup_test.sh` | `deploy_local.sh -c` cleans the archive through the manifest and leaves other data |
| `Unit_Performance_ArchiveCleanup` | `performance_archive_cleanup_test.sh` | The same cleanup as the performance scripts run it |
| `Unit_NFS_ComposeOverride` | `nfs_compose_override_test.sh` | The compose override that mounts each NFS profile |
| `Unit_NFS_ServerDryRun` | `nfs_server_dry_run_test.sh` | What `nfs_server.sh` would run, without sudo |
| `Unit_NFS_Classify` | `nfs_classify_test.sh` | How the NFS pass classifies a replay |
| `Unit_NFS_Args` | `nfs_args_test.sh` | The NFS pass scripts refuse an option with no value |
| `Unit_LocalCI_BadNfsProfile` | `local_ci_bad_profile_test.sh` | `local_distributed_ci.sh` refuses a bad `--nfs-profile` before touching anything |

---

## Functional Tests

| CTest Name | Source File | What it covers |
|---|---|---|
| `Functional_ChronoGrapher_DestroyFiles` | `tests/functional/chrono-grapher/chrono_grapher_destroy_files_test.cpp` | Deleting a story's or chronicle's archive files; needs no deployment |

The other five functional tests are disabled (see above).

---

## Tests That Need a Running Deployment

### Integration

| CTest Name | Source File | What it covers |
|---|---|---|
| `Integration_Client_ConnectRpc` | `tests/integration/client/client_connect_rpc_test.cpp` | Connect to ChronoVisor |
| `Integration_Client_MetadataRpc` | `tests/integration/client/client_metadata_rpc_test.cpp` | Create and destroy chronicles and stories |
| `Integration_Client_MultiPthread` | `tests/integration/client/client_multi_pthread_test.cpp` | POSIX threads sharing one client |
| `Integration_Client_ThreadInterdependency` | `tests/integration/client/client_thread_interdependency_test.cpp` | Threads that depend on each other's stories |
| `Integration_Client_MultiStorytellers` | `tests/integration/client/client_multi_storytellers_test.cpp` | Several story handles writing at once |
| `Integration_Client_StoryReader` | `tests/integration/client/client_story_reader_test.cpp` | Replaying events a writer logged |
| `Integration_Client_ReplayCallback` | `tests/integration/client/client_replay_callback_test.cpp` | The callback form of `ReplayStory` |
| `Integration_Client_ClientIdRoundtrip` | `tests/integration/client/client_clientid_roundtrip_test.cpp` | A writer's `ClientId` survives the archive and comes back in a replay |
| `Integration_Client_MultiArgobots` | `tests/integration/client/client_multi_argobots_test.cpp` | Argobots threads sharing one client |
| `Integration_Client_MultiOpenmp` | `tests/integration/client/client_multi_openmp_test.cpp` | OpenMP threads writing in parallel |
| `Integration_Client_HybridArgobots` | `tests/integration/client/client_hybrid_argobots_test.cpp` | Argobots and POSIX threads together |
| `Integration_KeeperGrapher_StoryChunkIngest` | `tests/integration/keeper-grapher/keeper_grapher_ingest_test.cpp` | Keeper ingestion into the grapher |
| `Integration_KeeperGrapher_StoryChunkExtract` | `tests/integration/keeper-grapher/keeper_grapher_extract_test.cpp` | Chunk extraction from keeper to grapher |
| `Integration_ChronoKVS_PluginIntegration` | `tests/integration/chronokvs/chronokvs_integration_test.cpp` | ChronoKVS writes and reads (300 s timeout) |
| `Integration_ChronoPubSub_PluginIntegration` | `tests/integration/chronopubsub/chronopubsub_integration_test.cpp` | ChronoPubSub publish and subscribe (600 s timeout) |
| `Integration_ChronoSQL_PluginIntegration` | `tests/integration/chronosql/chronosql_integration_test.cpp` | ChronoSQL on a small YCSB-shaped workload (600 s timeout) |

### End-to-end

| CTest Name | Source File | What it covers |
|---|---|---|
| `EndToEnd_TailRead_Playback` | `tests/end-to-end/tail-read/tail_read_playback_test.cpp` | `StoryHandle::playback(N)` returns the newest N events from the keepers |
| `EndToEnd_DestructiveAPIs_DestroyStory` | `tests/end-to-end/destructive-apis/destructive_apis_destroy_story_test.cpp` | Destroying a story removes its archive files |
| `EndToEnd_DestructiveAPIs_DestroyChronicle` | `tests/end-to-end/destructive-apis/destructive_apis_destroy_chronicle_test.cpp` | Destroying a chronicle removes every story's files |
| `EndToEnd_DestructiveAPIs_DestroyRefused` | `tests/end-to-end/destructive-apis/destructive_apis_destroy_refused_test.cpp` | A destroy is refused while another client holds the story |
| `EndToEnd_DestructiveAPIs_DisconnectAutoRelease` | `tests/end-to-end/destructive-apis/destructive_apis_disconnect_autorelease_test.cpp` | Disconnect releases the stories a client holds |
| `EndToEnd_DataIntegrity_StoryId`, `..._EventCount`, `..._EventOrder` | `tests/end-to-end/data-integrity/data_integrity_*_test.cpp` | Archived events match a reference input: story id, count and order |

`DestructiveAPIs_Setup`/`_Cleanup` and `DataIntegrity_Setup`/`_Cleanup` are CTest fixtures for these tests; they check the deployment but do not start one. `DataIntegrity_Setup` writes its input with `chrono-client-admin`, which the build no longer produces, so the data-integrity tests do not run. With the template's timings, the destructive-API tests can time out waiting for a story's first archive file, since the grapher's acceptance window alone is 60 s.

### Package discovery

| CTest Name | What it covers |
|---|---|
| `PackageDiscovery_CMakeFindPackage` | `find_package(ChronoLog)` in the build tree |
| `PackageDiscovery_PkgConfig` | `pkg-config chronolog` in the build tree |
| `PackageDiscovery_Installed_CMakeFindPackage` | `find_package(ChronoLog)` after install |
| `PackageDiscovery_Installed_PkgConfig` | `pkg-config` after install |

### Communication and overhead

| CTest Name | Source File | What it covers |
|---|---|---|
| `Communication_Thallium_ServerRepeaterRpc` | `tests/communication/thallium_server_test.cpp` | A Thallium RPC server |
| `Communication_Thallium_ClientSendRecvOrRdma` | `tests/communication/thallium_client_test.cpp` | Send/receive and RDMA against that server |
| `Communication_Thallium_ClientMpi` | `tests/communication/thallium_client_mpi_test.cpp` | The same from MPI ranks |
| `Communication_Thallium_ProtocolScript` | `tests/communication/thallium_protocol_test.sh` | Each supported Thallium transport |
| `Overhead_Clock_HighResolutionClock` | `tests/overhead/clock/clock_high_resolution_test.cpp` | Clock resolution and monotonicity |
| `Overhead_Clock_TimestampCollection` | `tests/overhead/clock/clock_timestamp_collection_test.cpp` | Cost of collecting timestamps |
| `Overhead_Clock_TimestampCollisionDetection` | `tests/overhead/clock/clock_timestamp_collision_test.cpp` | Timestamp collisions under concurrency |
| `Overhead_Clock_Script` | `tests/overhead/clock/clock_cset_shield_test.sh` | Clock cost with CPU-set shielding |
| `Overhead_Lock_ContentionBenchmark` | `tests/overhead/lock/lock_overhead_test.cpp` | Mutex and spinlock contention |

---

## Integration Scripts

These deploy real daemons on one host, run a scenario and stop the daemons. They take minutes and are not in CI; run them by hand before a pull request that touches the keeper's retention store, the watermark protocol, the replay path, the player or the keeper's write-ahead log.

```bash
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/watermark_loop_test.sh          # ~5 min, expect 5/5
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/watermark_replay_split_test.sh  # ~4 min, expect 9/9
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/replay_fanout_timing_test.sh    # ~3 min, expect 4/4
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/watermark_exact_split_test.sh   # ~3 min, expect 20/20
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/keeper_wal_crash_test.sh        # ~3 min, expect 8/8
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/keeper_wal_off_test.sh          # ~3 min, expect 8/8
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/keeper_restart_held_test.sh     # ~8 min, expect 11/11 on ofi+sockets, 8/11 on ofi+tcp
WORK_DIR=~/chronolog-install/chronolog \
  tests/integration/keeper_wal_group_test.sh        # ~2 min, expect 2/2
```

The first three `keeper_*` scripts run `chronolog-test-durable-probe`, which is built but not installed, so they need `BUILD_DIR`. `keeper_wal_group_test.sh` runs `chrono-bench` from the install tree. The durations of the four are estimates from the scripts' timing settings.

| Script | What it protects |
|---|---|
| `watermark_loop_test.sh` | Chunks are freed only once the grapher confirms them, none early, nothing lost across a grapher SIGSTOP, and the events are on disk afterwards. |
| `watermark_replay_split_test.sh` | A replay returns the same events whether they are on the keepers, archived, or split across both, with no duplicates. |
| `replay_fanout_timing_test.sh` | The player fetches from its keepers together: two keepers frozen with SIGSTOP cost one deadline, not two. |
| `watermark_exact_split_test.sh` | A replay splits exactly where the watermark puts it, while the data is on the keepers, archived, split, with the grapher stopped and after it resumes; one archive file per window, none rewritten. |
| `keeper_wal_crash_test.sh` | A keeper killed with SIGKILL right after it acknowledged durable writes ships every one of them to the archive once restarted, each exactly once in a replay, and the WAL segment is deleted after the grapher confirms. Accepted writes made just before the same kill are lost, which is the negative control. |
| `keeper_wal_off_test.sh` | A durable write to a keeper with the WAL off, and to a v3.3.0 keeper, gets `CL_ERR_DURABILITY_UNAVAILABLE` at once with no retry and stores nothing, while accepted writes to the same keepers still work. The v3.3.0 keeper is `V330_KEEPER`; if that file is missing, the script builds the `v3.3.0` tag on first use, which takes several minutes. |
| `keeper_restart_held_test.sh` | A keeper restarted while clients hold stories gets its pipelines back from the visor, accepts and durably stores new writes, is reached by the player's hot fetch, and the client reconnects at the same address; the durable events it acknowledged before the kill are recovered from its WAL. It runs once per protocol (the template's, then `ofi+tcp`). On `ofi+tcp` 3 of the 11 checks fail until two missing RPC deadlines are fixed: an accepted `record_event` to a dead keeper, and the visor's `start_story_recording` to it, wait without a deadline. |
| `keeper_wal_group_test.sh` | One `fdatasync` covers many durable writes: with 64 MPI ranks writing to one keeper whose RPC pool has 4 streams, a synced group holds more than 4 events. |

Run them one at a time; each deploys its own cluster on the same ports. Each restores the configuration it patched and stops its daemons on exit. If one is interrupted, check for leftover daemons and for a `*.backup` file in `~/chronolog-install/chronolog/conf/`.

---

## The Distributed Pipeline Locally

The pull-request check that runs a multi-host deployment is the distributed pipeline: 7 containers (1 visor, 2 keepers, 2 graphers, 2 players), with cross-host RPC, MPI clients on several containers and, at the end, the archive on an NFS export. `docker/local_distributed_ci.sh` runs the same stages against your working tree:

```bash
docker/local_distributed_ci.sh -i IMAGE                 # the same topology as CI
docker/local_distributed_ci.sh -i IMAGE -k 1 -g 1 -p 1  # 4 containers, faster
docker/local_distributed_ci.sh -i IMAGE --keep          # leave the containers up
docker/local_distributed_ci.sh -i IMAGE --nfs           # also run the NFS pass (needs sudo)
docker/local_distributed_ci.sh -i IMAGE --wal-restart      # also run stage 11c, the keeper WAL kill and restart
docker/local_distributed_ci.sh -i IMAGE --wal-restart-only # run stage 11c alone, skipping stages 7-11
```

`IMAGE` is the base image with Spack and the dependencies, as CI builds it (`ghcr.io/grc-iit/chronolog-base:sha-<hash>`). A full run with `--nfs` takes about 22 minutes. See [CI/CD](../guidelines/ci-cd.md#distributed-pipeline) for what each stage checks and what the NFS pass expects.

---

## AddressSanitizer

The normal Debug build has no sanitizer. Build a separate tree with the flags passed through the deploy script, install it, and run the unit tests:

```bash
tools/deploy/local_single_user_deploy.sh -b -t Debug -B ~/chronolog-build/Asan -I ~/chronolog-install-asan \
  --cxx-flags "-fsanitize=address -fno-omit-frame-pointer"
tools/deploy/local_single_user_deploy.sh -i -t Debug -B ~/chronolog-build/Asan -I ~/chronolog-install-asan

ASAN_OPTIONS=detect_leaks=1 ctest --test-dir ~/chronolog-build/Asan/Debug -R 'Unit_' --output-on-failure
```

Keep ASan and normal objects in separate build trees. As of 3.3.0, 26 unit tests fail under ASan, all with leaks: the StoryPipeline tests (its ingestion handle is never freed) and the ChronoKVS, ChronoPubSub and ChronoSQL configuration tests. Compare a run against that, not against a clean result.

---

## Workload Script

`tests/synthetic_workload/distributed_syslog.sh` is not registered with CTest. It ingests `/var/log/syslog` with `chrono-bench` from several nodes (the hosts in `conf/hosts_client`) against a running deployment.
