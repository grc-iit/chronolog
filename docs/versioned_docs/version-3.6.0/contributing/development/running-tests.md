---
sidebar_position: 4
title: "Running Tests"
---

# Running Tests

ChronoLog's tests come in three groups:

- **CTest entries** in the build tree: unit tests that need nothing running, and integration, end-to-end, communication and overhead tests that need a running deployment.
- **Integration scripts** in `tests/integration/*.sh` that deploy their own daemons, run a scenario and stop them. They are not registered with CTest.
- **The distributed pipeline**, a 7-container deployment that CI runs on every pull request and that `docker/local_distributed_ci.sh` runs locally. See [CI/CD](../guidelines/ci-cd.md).

A Debug build registers 1359 CTest entries: 1077 unit tests, 8 functional tests (5 disabled), 181 integration tests, 12 end-to-end tests and their fixtures, 4 package discovery tests, 72 communication tests and 5 overhead tests. `ctest -N` lists them for your build; a Release build has fewer, because some logger tests exist only in Debug.

---

## How to Run Tests

### Unit tests

Build through the deploy script, then run CTest in the build directory. The script builds into `<build dir>/<build type>`, so the path ends in `Debug`:

```bash
tools/deploy/local_single_user_deploy.sh -b -t Debug
tools/deploy/local_single_user_deploy.sh -i -t Debug

ctest --test-dir ~/chronolog-build/Debug -R 'Unit_' --output-on-failure -j4
```

The unit tests need no running daemons. Neither do the communication tests and the integration tests named `Integration_Client*Deadline`, `Integration_ChronologNativeRuntime`, `Integration_ChronologRegistryLifecycle` and `Integration_ChronologRpcColdLifecycle`: they run their own engines in the test process, over both `ofi+sockets` and `ofi+tcp` on loopback (set `FI_TCP_IFACE=lo` if the tcp provider picks another interface). Run them with the unit tests: `ctest -R 'Unit_|Communication_|Integration_(Client[A-Za-z]+Deadline|Chronolog)'`. A few bind a fixed loopback port (`Unit_ChronoCommon_ExtractionChainConfig` uses 2225), so stop a local deployment or another test run that holds it. Some binaries get their library paths only at install, so install before running CTest.

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
| `Unit_ChronoCommon_StoryChunk_` | 52 | `chrono_common_story_chunk_test.cpp` | Inserting, merging, erasing and extracting events in a StoryChunk, including window boundaries and duplicates |
| `Unit_ChronoCommon_StoryPipeline_` | 38 | `chrono_common_story_pipeline_test.cpp` | The story pipeline's timeline: chunk allocation, ingestion, decay, prepend/append, finalize, receipts and incarnation numbers |
| `Unit_ChronoCommon_ArchiveManifest` | 34 | `chrono_common_archive_manifest_test.cpp` | Manifest records, the writer's appends and the reader's tail, including a log replaced on the same inode |
| `Unit_ChronoCommon_ChunkWriter`, `Unit_ChronoCommon_ArchiveLayout`, `Unit_ChronoCommon_FileSync` | 23 | `chrono_common_story_chunk_writer_test.cpp` | Writing a window through a temporary name into its story directory; name encoding and file names |
| `Unit_ChronoCommon_ConfTemplate` | 7 | `chrono_common_conf_template_test.cpp` | The shipped configuration template keeps the timing rules between components |
| `Unit_ChronoCommon_DataStoreConf` | 6 | `chrono_common_data_store_conf_test.cpp` | Validation of the DataStoreInternals settings |
| `Unit_ChronoCommon_ExtractionModule` | 5 | `chrono_common_extraction_module_test.cpp` | The drain loop shared by the keeper and the grapher |
| `Unit_ChronoCommon_LogEventCodec` | 5 | `chrono_common_log_event_codec_test.cpp` | The byte layout of a keeper WAL event, and how a cut, oversized or newer-version encoding is refused |
| `Unit_ChronoCommon_ChunkSenderAnswer` | 6 | `chrono_common_chunk_sender_answer_test.cpp` | The answer an RPC handler gives its caller: a thallium error from it does not leave the handler, and the grapher's, the player's and the keeper's handlers answer only through it |
| `Unit_ChronoCommon_RDMATransferAgentLifetime` | 1 | `chrono_common_rdma_transfer_agent_lifetime_test.cpp` | Deleting a keeper's chunk sender for one grapher leaves its sender for another grapher on the same provider id working |
| `Unit_ChronoCommon_Crc32c` | 3 | `chrono_common_crc32c_test.cpp` | The SSE4.2 path and the boost fallback of CRC-32C agree |
| `Unit_ChronoCommon_ChunkExtractorCSV` | 2 | `chrono_common_chunk_extractor_csv_test.cpp` | The CSV extractor and empty windows |
| `Unit_ChronoCommon_ExtractionChainConfig` | 1 | `chrono_common_extraction_chain_config_test.cpp` | Parsing extraction chain configurations (custom `main`, not GTest) |
| `Unit_ChronoCommon_ManagedBarrier`, `..._PartialPlayerTerminal` | 13 | `managed_stop_barrier_test.cpp` | The managed stop's barrier: a component may finish its stop only once every data gate, ready record and native-cleanup acknowledgement it depends on is complete |
| `Unit_ChronoCommon_LifecyclePublisher` | 6 | `lifecycle_publisher_test.cpp` | Writing a daemon's lifecycle status record within a deadline: an expired write is not started, a failed one is retried only for a new request, a terminal one waits for the write in progress |
| `Unit_ChronoCommon_ExtractionConfigurationOwnership` | 5 | `extraction_configuration_ownership_test.cpp` | Copying and moving an extraction configuration keeps each extractor alive and releases it once |
| `Unit_ChronoCommon_ExtractionLifecycle` | 1 | `extraction_lifecycle_test.cpp` | A repeated shutdown waits for the same blocked extractor and joins it once |

### ChronoKeeper (`tests/unit/chrono-keeper/`)

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronoKeeper_KeeperChunkRetentionStore` | 67 | `chrono_keeper_retention_store_test.cpp` | The keeper's retention store: the tail index, retained chunks, and freeing a chunk only once the grapher has written it |
| `Unit_ChronoKeeper_KeeperWalTest` | 49 | `chrono_keeper_wal_test.cpp` | The WAL committer: group commit, a failed write or sync and the erase of the group, rotation, a retry joining an attempt, release and segment deletion in any order, stop, and a destroy after the WAL failed or in a segment deleted before an older one |
| `Unit_ChronoKeeper_WalRecoveryTest` | 37 | `chrono_keeper_wal_recovery_test.cpp` | Reading a WAL at startup: log order, torn tails, bad records in the last group and in earlier ones, the copy of a zeroed tail, unknown versions, dropped stories and `dropped-unlogged` |
| `Unit_ChronoKeeper_WalFileSinkTest` | 22 | `chrono_keeper_wal_file_sink_test.cpp` | The WAL's file layer and its fault-injecting test sink: `flock`, zeroed files, `writev`, renames, directory sync, held and failed syncs |
| `Unit_ChronoKeeper_WalSegmentManagerTest` | 17 | `chrono_keeper_wal_segment_manager_test.cpp` | Segment headers, the spare prepared ahead on its own stream, adopting a header-only segment, and an idle preparer that does not spin |
| `Unit_ChronoKeeper_KeeperDurableWriter` | 19 | `chrono_keeper_durable_writer_test.cpp` | The durable write path: its checks and their order, the cap on unconfirmed chunks, a pipeline a durable write started kept while events arrive, the pipeline it starts for an unknown story, ingestion only after the sync, a retry that joins an earlier attempt, and shutdown |
| `Unit_ChronoKeeper_WalFormat` | 14 | `chrono_keeper_wal_format_test.cpp` | The record frames: length, CRC32C, segment header and group record, and how a cut or flipped frame is read |
| `Unit_ChronoKeeper_KeeperWalConf` | 12 | `chrono_keeper_wal_conf_test.cpp` | Parsing and validating the `WAL` configuration block |
| `Unit_ChronoKeeper_WalMountCheck` | 11 | `chrono_keeper_wal_mount_check_test.cpp` | The startup warnings about the WAL directory: NFS, tmpfs, overlay, the root device and low space |
| `Unit_ChronoKeeper_WalReleaseTracker` | 9 | `chrono_keeper_wal_release_tracker_test.cpp` | The table of pending durable events: release by a confirmed chunk, copies in two segments, and which segments can be deleted |
| `Unit_ChronoKeeper_KeeperReleaseDataLoss`, `..._KeeperOrphanAtRetirementDataLoss` | 6 | `chrono_keeper_release_dataloss_test.cpp` | Releasing a story does not lose events still in its pipeline |
| `Unit_ChronoKeeper_KeeperExtractionChain` | 4 | `chrono_keeper_extraction_chain_test.cpp` | The chain hands extracted chunks to the retention store instead of deleting them |
| `Unit_ChronoKeeper_KeeperWalReplay` | 5 | `chrono_keeper_wal_replay_test.cpp` | Recovered events become sealed chunks aligned to the chunk duration, under the story id taken from the names, and stay retained until the grapher confirms them |
| `Unit_ChronoKeeper_KeeperOrphanRecovery` | 3 | `chrono_keeper_orphan_recovery_test.cpp` | Events that arrive for a story with no pipeline are sealed and kept |
| `Unit_ChronoKeeper_KeeperChunkRetentionCap` | 2 | `chrono_keeper_retention_cap_test.cpp` | The `retention_cap_mb` alarm |
| `Unit_ChronoKeeper_KeeperStoryRetirement` | 2 | `chrono_keeper_story_retirement_test.cpp` | A retired story frees its chunks only when they are written and out of the tail |
| `Unit_ChronoKeeper_KeeperRpcPolicy` | 3 | `rpc_policy_configuration_test.cpp` | The keeper's `RpcPolicy` block: defaults, overrides and refused values |

### ChronoGrapher (`tests/unit/chrono-grapher/`)

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronoGrapher_StoryWatermarkRegistry` | 36 | `story_watermark_registry_test.cpp` | The per-story persisted watermark W and the receipts of delivered keeper chunks |
| `Unit_ChronoGrapher_HDF5FileChunkExtractorWatermark` | 27 | `hdf5_file_chunk_extractor_test.cpp` | The HDF5 extractor advances W and settles receipts only after a window and its manifest record are written |
| `Unit_ChronoGrapher_GrapherConfiguration` | 8 | `grapher_configuration_test.cpp` | A grapher refuses a chain without the HDF5 extractor |
| `Unit_ChronoGrapher_WatermarkReportTransport` | 9 | `watermark_report_transport_test.cpp` | Watermark reports from the grapher to the keepers over real engines on loopback |
| `Unit_ChronoGrapher_GrapherDestroy`, `..._ExtractionQueue`, `..._GrapherExtractionChain` | 5 | `grapher_destroy_test.cpp` | A destroy waits for windows still being written |
| `Unit_ChronoGrapher_GrapherDataCollectionReport` | 2 | `grapher_data_collection_report_test.cpp` | Watermark reports sent from the data-collection loop |
| `Unit_ChronoGrapher_GrapherPipelineRetirement` | 2 | `grapher_pipeline_retirement_test.cpp` | A stopped story's pipeline writes its last windows before it is retired |
| `Unit_ChronoGrapher_Native/WatermarkRpcDeadline`, `..._StoryWatermarkPublication` | 48 | `watermark_rpc_deadline_test.cpp` | Watermark reports under deadlines, over both transports: a report kept pending across attempts, capacity refusals, an expired preparation never sent, the final report at a stop, and native owners kept until the send settles |
| `Unit_ChronoGrapher_GrapherRpcPolicy` | 3 | `rpc_policy_configuration_test.cpp` | The grapher's `RpcPolicy` block: defaults, overrides and refused values |

### ChronoPlayer (`tests/unit/chrono-player/`)

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronoPlayer_ArchiveManifestIndex` | 22 | `chrono_player_archive_manifest_index_test.cpp` | The player finds archive files only through the manifest, and drops the files of destroyed stories |
| `Unit_ChronoPlayer_HotRangeSplit`, `Unit_ChronoPlayer_ReplayPlan` | 14 | `chrono_player_hot_range_split_test.cpp` | Where a replay splits between the archive and the keepers, and where a replay that starts at 0 starts |
| `Unit_ChronoPlayer_ArchiveNumberedFiles` | 5 | `chrono_player_archive_numbered_files_test.cpp` | A window written more than once is read from every file |
| `Unit_ChronoPlayer_Transports/KeeperHotFetch`, `..._HotFetchPhaseDeadline` | 17 | `chrono_player_keeper_hot_fetch_client_test.cpp` | Fetches from the keepers in waves of two within one deadline, over both transports; a silent keeper costs one deadline and drops out of the split |
| `Unit_ChronoPlayer_ReplayEventMerge` | 4 | `chrono_player_replay_event_merge_test.cpp` | Merging archive and keeper events without duplicates |
| `Unit_ChronoPlayer_ArchiveRequestReply`, `..._QueryResponseAgentLifetime` | 3 | `chrono_player_archive_request_reply_test.cpp` | An archive read the reading agent cannot use still answers its query, short; deleting the response agent of one client leaves another client's agent on the same provider id working |
| `Unit_ChronoPlayer_ArchiveClientId` | 2 | `chrono_player_archive_client_id_test.cpp` | The full 64-bit client id survives the archive |
| `Unit_ChronoPlayer_ArchiveVisibility` | 2 | `chrono_player_archive_visibility_test.cpp` | Archive reads at the split point |
| `Unit_ChronoPlayer_PlayerReaderConf` | 2 | `chrono_player_reader_conf_test.cpp` | The `ArchiveReaders` block; removed settings are ignored |
| `Unit_ChronoPlayer_PlayerPipelineRetirement` | 2 | `player_pipeline_retirement_test.cpp` | A stopped story still answers queries until it is retired |
| `Unit_ChronoPlayer_HDF5ArchiveReadingAgent` | 1 | `chrono_player_hdf5_archive_reader_test.cpp` | Reading story chunks from HDF5 files (custom `main`, not GTest) |
| `Unit_ChronoPlayer_PlayerQueries` | 3 | `player_query_quiescence_test.cpp` | Closing the player's query entries refuses new queries without sending anything and waits for those already admitted |
| `Unit_ChronoPlayer_PlayerRpcPolicy` | 3 | `rpc_policy_configuration_test.cpp` | The player's `RpcPolicy` block: defaults, overrides and refused values |

### ChronoVisor (`tests/unit/chrono-visor/`)

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronoVisor_StoryGroupSelection`, `..._ActiveGroups`, `..._ActiveRecordingGroup` | 12 | `story_group_selection_test.cpp` | A story's recording group comes from its story id, stays the same when it is acquired again, and moves only when the groups change |
| `Unit_ChronoVisor_KeeperRestartStories` | 6 | `keeper_restart_stories_test.cpp` | The stories the visor starts on a registering keeper: only its recording group's active stories, and a stop for one released or moved during the start |
| `Unit_ChronoVisor_ChronicleStoryId` | 3 | `chronicle_story_id_test.cpp` | Name pairs that concatenate alike get different story ids |
| `Unit_ChronoVisor_Native/AdminDeadline`, `..._Native/RegistryShutdownOwnership`, `..._VisorLifecycleAdmission`, `..._VisorOwnedRuntime` | 71 | `admin_deadline_test.cpp` | ChronoVisor's calls to the keepers, graphers and players under deadlines, over both transports: silent and slow keepers, the roster wait, unconfirmed starts and stops settled by the next acquire or release, held stories started in waves, an admin client deleted or replaced while others keep working (#743), a departed keeper's client freed, and shutdown ownership |
| `Unit_ChronoVisor_VisorRpcPolicy` | 3 | `rpc_policy_configuration_test.cpp` | ChronoVisor's `RpcPolicy` block: defaults, overrides and refused values |

### Client library (`tests/unit/chronolog-client/`)

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronologClient_Transports/DurableDeadline` | 18 | `durable_deadline_test.cpp` | Durable and accepted writes through the client's RPC runtime, over both transports, against a keeper in a child process: keeper codes, a resend with the same identity, the whole budget, one slot per write, and slots given back |
| `Unit_ChronologClient_Transports/TailDeadline`, `..._TailPhaseBudget` | 13 | `tail_deadline_test.cpp` | Tail reads within one `tail_ms`: waves of two keepers, a failed keeper gives a partial result and its slot back, both keepers timing out |
| `Unit_ChronologClient_ChronologTimer`, `..._ChronologClockOwnership`, `..._StorytellerEventIndex` | 12 | `chronolog_timer_test.cpp` | Strictly increasing timestamps, also across a clock step back and a reconnect; clock failures refused before a send; exhaustion; the event index |
| `Unit_ChronologRpc_ClientRpcPolicy`, `..._RpcPolicy`, `..._RpcPolicyContext` | 10 | `rpc_policy_test.cpp` | Loading and validating the client's `RpcPolicy`, and the policy frozen at the first `Connect` |
| `Unit_ChronologClient_MetadataOperationState`, `..._RpcPolicyContext` | 4 | `metadata_operation_state_test.cpp` | An unconfirmed metadata change blocks conflicting ones until a later answer resolves it |
| `Unit_ChronologClient_ReplayCallbacks` | 2 | `replay_deadline_test.cpp` | A replay callback is never called after the budget runs out, and closing waits for one already running |
| `Unit_ChronologClient_AppendCompatibility` | 1 | `append_deadline_test.cpp` | A `StoryHandle` subclass written for the old API still gets an acknowledgement and an unconfirmed fallback from `append_event` |
| `Unit_ChronologClient_PlaybackQueryRpcClientLifetime` | 1 | `playback_query_rpc_client_lifetime_test.cpp` | Deleting a reader's playback route to one player leaves its route to another player on the same provider id working |
| `Unit_ChronologClient_QueryServiceAddressTest` | 10 | `query_service_address_test.cpp` | The address a reader listens on when the visor is on another host |
| `Unit_ChronologClient_ClientIdentityTest` | 5 | `chronolog_client_identity_test.cpp` | The `ClientId` layout |
| `Unit_ChronologClient_ClientLoginName` | 4 | `client_login_name_test.cpp` | The logged user name when the process has no login name |
| `Unit_ChronologClient_LogEventOptions` | 4 | `client_log_event_options_test.cpp` | The defaults of `LogEventOptions`, and the default `log_event(record, options, event_time)` of a `StoryHandle` subclass |
| `Unit_ChronologClient_ClientErrcode` | 2 | `client_errcode_durability_test.cpp` | The durability error codes and the names of all codes; reserved codes keep their meaning |
| `Unit_PyClient_DurableBinding` | 1 | `py_client_durable_binding_test.py` | The Python binding of `Durability`, `LogEventOptions` and `log_event(record, options)`; a Python script, registered only when the Python module is built |

### RPC runtime (`tests/unit/chrono-rpc/`)

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Unit_ChronologRpc_RpcRuntime`, `..._RpcRuntimeOwner` | 47 | `rpc_runtime_test.cpp` | Admitting calls into a fixed set of slots, one deadline per call, keeping unanswered work and its slot, giving slots back (also to back-to-back calls), close and drain |
| `Unit_ChronologRpc_RpcDeadline` | 5 | `rpc_deadline_test.cpp` | Deadline arithmetic: the entry time kept, attempts capped, invalid budgets refused, no overflow |
| `Unit_ChronologLifecycle_LifecycleJoin` | 2 | `lifecycle_record_test.cpp` | A lifecycle step's join survives a caller's expiry |

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
| `Unit_Deploy_LocalWal` | `deploy_local_wal_test.sh` | `deploy_local.sh --wal` in the generated keeper confs, the checks on `--restart-keeper` and `--force`, and the `--stop` table of exit summaries |
| `Unit_Deploy_ClusterWal` | `deploy_cluster_wal_test.sh` | The keeper WAL handling of `deploy_cluster.sh --stop`, `--clean` and `--restart-keeper` against made-up keeper hosts |
| `Unit_Deploy_WalDirProbe` | `wal_dir_probe_test.sh` | `wal_dir_probe.sh` choosing a WAL directory on made-up hosts, and `deploy_cluster.sh` writing the choice into a keeper's conf |
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
| `Functional_ChronoPlayer_Transports/PlaybackStartup` (2) | `tests/functional/chrono-player/chrono_player_playback_service_startup_test.cpp` | The player's playback service keeps the first registered callback when a later initialization fails, over both transports; needs no deployment |

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

These need no deployment; each runs its own engines in the test process, over both transports:

| CTest prefix | Cases | Source File | What it covers |
|---|---:|---|---|
| `Integration_ClientMetadataDeadline_` | 52 | `tests/integration/client/client_metadata_deadline_test.cpp` | Connect, disconnect and the metadata calls within `control_ms`: unconfirmed outcomes kept without a retry, capacity, concurrent callers with their own budgets |
| `Integration_ClientReplayDeadline_` | 40 | `tests/integration/client/client_replay_deadline_test.cpp` | `ReplayStory` within `replay_ms`: admission, the player's answer, bad acknowledgements, callbacks |
| `Integration_ClientAppendDeadline_` | 38 | `tests/integration/client/client_append_deadline_test.cpp` | Accepted appends within `append_ms`: the event identity on the wire, refusals, late and lost replies, expiry at each stage, clock failures |
| `Integration_ClientAcquireDeadline_` | 15 | `tests/integration/client/client_acquire_deadline_test.cpp` | `AcquireStory` against ChronoVisor's portal: the confirmed roster, saturation, lost replies |
| `Integration_ChronologRpcColdLifecycle_` | 10 | `tests/unit/chrono-rpc/rpc_native_cold_lifecycle_probe.cpp` | Starting and retiring native engines: blocked constructors and cleanup, two engines retired in either order |
| `Integration_ChronologNativeRuntime_` | 4 | `tests/unit/chrono-rpc/native_runtime_test.cpp` | The native runtime keeps its constructor and cleanup after a caller expires, and several engines share one generation |
| `Integration_ChronologRegistryLifecycle_` | 2 | `tests/unit/chrono-keeper/registry_lifecycle_test.cpp` | A registration that expires does not replace the live native call |

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
| `Communication_RpcClientBulk_*` (24) | `tests/communication/rpc_client_bulk_test.cpp` | The client answering the player: busy and failed answers, callbacks, staged expiry; both transports |
| `Communication_RpcBulkLifetime_*` (22) | `tests/communication/rpc_bulk_lifetime_test.cpp` | A keeper's chunk send: capacity, large chunks, an expired preparation, identity and route of a resend; both transports |
| `Communication_RpcBulkReceiver_*` (8) | `tests/communication/rpc_bulk_receiver_test.cpp` | The grapher's receiver: busy when its two receive slots are taken, decode and reply errors |
| `Communication_RpcChunkReceive_*` (8) | `tests/communication/rpc_chunk_receive_test.cpp` | Pulling a chunk in pieces, its checksum, duplicates and pull errors |
| `Communication_RpcPlayerResponse_*` (4) | `tests/communication/rpc_player_response_test.cpp` | The player's hot fetch and answer, also with three keepers |
| `Communication_RpcDeadlineCapability_*` (2) | `tests/communication/rpc_deadline_capability_probe.cpp` | Timed forwards and bulk transfers against a delayed peer, with the caller and the drain bounded, on each transport |
| `Overhead_Clock_HighResolutionClock` | `tests/overhead/clock/clock_high_resolution_test.cpp` | Clock resolution and monotonicity |
| `Overhead_Clock_TimestampCollection` | `tests/overhead/clock/clock_timestamp_collection_test.cpp` | Cost of collecting timestamps |
| `Overhead_Clock_TimestampCollisionDetection` | `tests/overhead/clock/clock_timestamp_collision_test.cpp` | Timestamp collisions under concurrency |
| `Overhead_Clock_Script` | `tests/overhead/clock/clock_cset_shield_test.sh` | Clock cost with CPU-set shielding |
| `Overhead_Lock_ContentionBenchmark` | `tests/overhead/lock/lock_overhead_test.cpp` | Mutex and spinlock contention |

---

## Integration Scripts

These deploy real daemons on one host, run a scenario and stop the daemons. They take minutes, so CI runs them every night instead of on each pull request (see [Nightly Integration](../guidelines/ci-cd.md#nightly-integration)); run them by hand before a pull request that touches the keeper's retention store, the watermark protocol, the replay path, the player or the keeper's write-ahead log.

```bash
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/watermark_loop_test.sh          # ~5 min, expect 8/8
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/watermark_replay_split_test.sh  # ~4 min, expect 9/9
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/replay_fanout_timing_test.sh    # ~3 min, expect 4/4
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/watermark_exact_split_test.sh   # ~3 min, expect 20/20
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/keeper_wal_crash_test.sh        # ~4 min, expect 11/11
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/keeper_wal_off_test.sh          # ~3 min, expect 5/5
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/keeper_restart_held_test.sh     # ~8 min, expect 22/22 (11 per transport)
WORK_DIR=~/chronolog-install/chronolog \
  tests/integration/keeper_wal_group_test.sh        # ~2 min, expect 2/2
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/keeper_wal_cap_test.sh          # ~3 min, expect 5/5
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/keeper_wal_shutdown_test.sh     # ~2 min, expect 5/5
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/keeper_wal_startup_test.sh      # ~3 min, expect 6/6
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/keeper_sigterm_stop_test.sh     # ~1 min, expect 3/3
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/rpc_bulk_timeout_test.sh        # ~1 min, expect 58/58
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/rpc_deadline_matrix_test.sh     # ~5 min, expect 130/130
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/rpc_lifecycle_deadline_test.sh  # ~2 min, expect 2/2 (one per protocol in PROTOCOLS)
WORK_DIR=~/chronolog-install/chronolog \
  tests/integration/keeper_leave_test.sh            # ~1 min, expect 8/8
```

`keeper_wal_crash_test.sh`, `keeper_wal_off_test.sh`, `keeper_restart_held_test.sh`, `keeper_wal_cap_test.sh`, `keeper_wal_shutdown_test.sh` and `keeper_wal_startup_test.sh` run `chronolog-test-durable-probe`, which is built but not installed, so they need `BUILD_DIR`. The startup script also runs `chronolog-test-wal-fixture` to write its WAL directories, the crash script runs it to count the E records in the keeper's WAL, and the shutdown script preloads `libchronolog_wal_sync_hold.so`, both from the same build directory. `keeper_wal_group_test.sh` runs `chrono-bench` from the install tree. The durations are estimates from the scripts' timing settings.

`tests/integration/run_integration_tests.sh`, which the nightly workflow runs, runs them one at a time:

```bash
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/run_integration_tests.sh                       # every *_test.sh, ~30 min
WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
  tests/integration/run_integration_tests.sh keeper_wal_crash_test keeper_wal_cap_test
```

Between scripts it stops any daemon left running from `WORK_DIR`'s `bin/`, and puts the installed conf template back if a script left it patched, which happens when the time limit kills a script before its own cleanup. It writes each script's output, the daemons' logs and a `summary.md` with one row per script to `LOG_DIR`, a new directory under `${TMPDIR:-/tmp}` unless set. `SCRIPT_TIMEOUT` (1800 s by default) limits each script, and other variables, such as `PROTOCOLS=ofi+tcp` for `keeper_restart_held_test.sh`, pass through to the scripts. It exits 0 only when every script passed.

| Script | What it protects |
|---|---|
| `watermark_loop_test.sh` | Chunks are freed only once the grapher confirms them, none early, nothing lost across a grapher SIGSTOP, and the events are on disk afterwards. |
| `watermark_replay_split_test.sh` | A replay returns the same events whether they are on the keepers, archived, or split across both, with no duplicates. |
| `replay_fanout_timing_test.sh` | The player fetches from its keepers together: two keepers frozen with SIGSTOP cost one deadline, not two. |
| `watermark_exact_split_test.sh` | A replay splits exactly where the watermark puts it, while the data is on the keepers, archived, split, with the grapher stopped and after it resumes; one archive file per window, none rewritten. |
| `keeper_wal_crash_test.sh` | A keeper killed with SIGKILL right after it acknowledged durable writes ships every one of them to the archive once restarted, each exactly once in a replay, and the WAL segment is deleted after the grapher confirms. Before that archive write, a replay already returns them from the restarted keeper, and sending the same identities again does not store a second copy. Accepted writes made just before the same kill are lost, which is the negative control. |
| `keeper_wal_cap_test.sh` | With the grapher stopped and `durable_cap_mb` at 1, durable writes that sealed into an unconfirmed chunk make the next durable write return `CL_ERR_DURABILITY_FULL` with nothing stored. After the grapher runs, durable writes are admitted again and every acknowledged identity is replayed once. |
| `keeper_wal_shutdown_test.sh` | A durable write observed inside its group `fdatasync` is still answered `CL_SUCCESS` when the keeper is sent SIGTERM, a durable write after that gets `CL_ERR_DURABILITY_UNAVAILABLE` and is not stored, and the exit summary reports the events the grapher had not confirmed. |
| `keeper_wal_startup_test.sh` | A keeper whose older WAL segment contains a bad record exits without changing the files. A bad record in the last group, with an intact record after it, is copied aside and zeroed, the keeper stays up, and a replay returns the earlier event and not the intact one that followed the bad record. |
| `keeper_wal_off_test.sh` | A durable write to a keeper with the WAL off gets `CL_ERR_DURABILITY_UNAVAILABLE` at once with no retry and stores nothing, while accepted writes to the same keeper still work. It no longer deploys a v3.3.0 keeper: mixed-version deployments are not supported. |
| `keeper_restart_held_test.sh` | A keeper restarted while clients hold stories gets its pipelines back from the visor, accepts and durably stores new writes, is reached by the player's hot fetch, and the client reconnects at the same address; the durable events it acknowledged before the kill are recovered from its WAL. It runs the template's protocol and then the other one, 11 checks each; `PROTOCOLS` overrides the list. |
| `keeper_wal_group_test.sh` | One `fdatasync` covers many durable writes: with 64 MPI ranks writing to one keeper whose RPC pool has 4 streams, a synced group holds more than 4 events. |
| `keeper_sigterm_stop_test.sh` | A keeper sent one SIGTERM while its last write is unconfirmed: its first stop attempt runs out with the grapher's confirmation still pending, and it exits by itself once the grapher confirms, inside `shutdown_confirm_timeout_secs`, with no second SIGTERM. |
| `rpc_bulk_timeout_test.sh` | Runs the `Communication_RpcBulkLifetime_`, `RpcBulkReceiver_`, `RpcPlayerResponse_` and `RpcClientBulk_` cases one at a time on both transports. Deploys no daemons. |
| `rpc_deadline_matrix_test.sh` | Runs the `Integration_Client{Append,Metadata,Replay}Deadline` cases one at a time on both transports, compiles a program against the installed client headers, and checks a real `ReplayStory` that times out at the shipped 180 s budget. Deploys no daemons. |
| `rpc_lifecycle_deadline_test.sh` | With a keeper paused, a public tail read stays within its budget, against real daemons; once per protocol in `PROTOCOLS`. |
| `keeper_leave_test.sh` | With two keepers in a recording group, stopping one with SIGTERM leaves the other usable: a new story is acquired, written, released and destroyed with its chronicle, a second one too, and the visor logs no refused start, no unconfirmed stop and no failed admin client. Before #743 every acquire after the keeper left returned `CL_ERR_NO_KEEPERS`. It uses the template's ports and refuses to start while any ChronoLog daemon runs on the host. |

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

Keep ASan and normal objects in separate build trees. The unit suite passes clean under ASan with `detect_leaks=1`, so any failure is new. A checkout of 3.5.0 or older still has 27 failures, none an AddressSanitizer error: the StoryPipeline tests, the plugins' configuration tests and `Unit_PyClient_DurableBinding`.

---

## Workload Script

`tests/synthetic_workload/distributed_syslog.sh` is not registered with CTest. It ingests `/var/log/syslog` with `chrono-bench` from several nodes (the hosts in `conf/hosts_client`) against a running deployment.
