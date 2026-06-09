# ChronoLog Test-Driven Development Spine

## Purpose

This file defines the test spine for the May 31 ChronoLog completion effort. It is not a full test suite. It is the ordered set of contract tests that should be written first, slice by slice, before implementing each capability.

The working rule is:

> For every major capability, write one focused failing contract test first, implement the minimum code to pass it, then add edge-case and integration tests before calling the capability done.

Do not add all of these tests as permanently failing CTest targets at once. Add them slice by slice so the repository stays buildable while development proceeds.

## Test Organization

Use the existing repo test layout:

- `tests/unit/<component>/` for deterministic logic with fake clocks, fake stores, and temp directories.
- `tests/functional/<component>/` for component-level service behavior with local objects or bounded service harnesses.
- `tests/integration/<area>/` for RPC/client/server behavior that requires running components.
- `tests/end-to-end/` for full local deployment flows.
- `tests/performance/` or `tools/benchmark/` for benchmark runs and result extraction.

Proposed new folders when needed:

- `tests/unit/chrono-clock/`
- `tests/unit/chrono-keeper/`
- `tests/unit/chrono-replay/`
- `tests/integration/playback/`
- `tests/integration/replay/`
- `tests/end-to-end/application-logging/`

## Global Testing Rules

- Prefer fake clocks over sleeps.
- Prefer temp directories over `/tmp` defaults.
- Assert exact error classes instead of generic failure.
- Assert invariants after every state mutation in unit tests.
- Keep integration tests bounded by explicit timeouts.
- Mark long-running or environment-dependent tests `MANUAL`.
- Add one negative test for every new success path.
- Add one restart/failure test for every new durability or archival claim.

## Capability Spine

### 1. Clock Sync And ChronoTick

First failing test:

- `Unit_ChronoClock_SyncStateComputesComparableTicks`

Contract:

- Given one Visor clock base and two clients with different local offsets, both clients compute ChronoTicks in the same comparable time domain.
- Later Visor time produces a larger ChronoTick.

Expected initial failure:

- No first-class `ChronoTick` or clock sync state exists, and client code uses raw local `high_resolution_clock`.

Follow-on tests:

- `Unit_ChronoClock_RoundTripLatencyProducesATWInput`
- `Unit_ChronoClock_RejectsInvalidClockState`
- `Integration_ClientConnect_ReturnsClockSyncState`
- `Integration_ClientSyncClock_RefreshesClockState`

Risks:

- Unit confusion between ns/us/ms/sec.
- Wall-clock flakiness.
- Breaking old connect wire protocol.

Mitigations:

- Use explicit chrono duration types or typed wrappers.
- Test with fake clocks only at unit level.
- Bump protocol version or add compatibility handling.

### 2. Keeper Hot Journal, Index, Tail, And Backlog

First failing test:

- `Unit_ChronoKeeper_HotStoreAppendUpdatesJournalIndexAndTail`

Contract:

- Appending an event stores it in the Keeper hot journal, makes it visible through range scan, and updates tail for that story.

Expected initial failure:

- Events currently land in ingestion deques/story chunks, not an explicit hot journal/index/tail module.

Follow-on tests:

- `Unit_ChronoKeeper_HotStoreFetchByEventId`
- `Unit_ChronoKeeper_HotStoreRangeScanSortedByChronoTick`
- `Unit_ChronoKeeper_HotStoreTailReturnsLatestTick`
- `Unit_ChronoKeeper_HotStoreCapacityReturnsBackpressure`
- `Unit_ChronoKeeper_BacklogWatermarkControlsEviction`
- `Functional_ChronoKeeper_RecordRpcCommitsToHotStore`

Risks:

- Journal, index, tail, and backlog diverge under updates.
- Locking becomes too coarse.
- Existing ingestion pipeline gets bypassed accidentally.

Mitigations:

- Put all mutations behind one hot-store API.
- Add invariant assertions in tests.
- Integrate hot store into existing flow before removing old paths.

### 3. Acceptance Time Window

First failing test:

- `Unit_ChronoKeeper_ATWRejectsOutsideWindowEvent`

Contract:

- An event whose ChronoTick is older than the acquisition/current ATW lower bound is rejected with a retryable backdated error and is not inserted.

Expected initial failure:

- Current Keeper pipeline prepends older chunks and expands acceptance window.

Follow-on tests:

- `Unit_ChronoKeeper_ATWAcceptsNearLateEvent`
- `Unit_ChronoKeeper_ATWRejectDoesNotUpdateTail`
- `Integration_RecordOutsideATW_ReturnsRetryableError`
- `EndToEnd_ApplicationLogging_ATWRejectionIsVisibleInMetrics`

Risks:

- Real clients may see unexpected rejects.
- Tests using wall time become flaky.
- ATW policy may be confused with chunk retention windows.

Mitigations:

- Make ATW configurable and observable.
- Use fake clocks in unit tests.
- Keep ATW validation in Keeper hot-store/record layer, not archive chunk logic.

### 4. Collision Policy

First failing test:

- `Unit_ChronoKeeper_CollisionOrderedListByArrival`

Contract:

- Two distinct events with the same ChronoTick under the ordered-list policy are both retained and returned in deterministic arrival order.

Expected initial failure:

- Current ordering uses `(eventTime, clientId, eventIndex)` and has no explicit policy metadata.

Follow-on tests:

- `Unit_ChronoKeeper_CollisionDuplicateReject`
- `Unit_ChronoKeeper_UnsupportedCollisionPolicyFailsClosed`
- `Integration_CreateChronicle_CollisionPolicyPropagatesToKeeper`

Risks:

- Same-tick behavior can break replay sorting.
- Unsupported policies may be silently accepted.

Mitigations:

- Store collision policy explicitly in acquisition metadata.
- Reject unsupported policies until implemented.
- Include event identity in replay dedupe logic.

### 5. Keeper Tail Playback

First failing test:

- `Functional_ChronoKeeper_PlaybackReturnsTailWithoutPlayer`

Contract:

- After records are accepted into Keeper hot state, `Playback` returns the latest event even when ChronoPlayer is not running and no HDF5 archive exists.

Expected initial failure:

- Public tail playback API and Keeper tail RPC do not exist.

Follow-on tests:

- `Unit_ChronoKeeper_HotStoreTailEmptyReturnsNoData`
- `Integration_ClientPlayback_ReturnsLatestEvent`
- `Integration_ClientPlayback_StoppedPlayerStillSucceeds`
- `EndToEnd_ApplicationLogging_TailMonitorSeesLatestEvent`

Risks:

- Existing "playback" names mean historical replay in current code.
- Tail can disagree with range replay if event identity differs.

Mitigations:

- Define terminology: Playback equals Keeper tail read; Replay equals historical/range read.
- Use shared event result types for playback and replay.

### 6. Safer Keeper-To-Grapher Transfer

First failing test:

- `Functional_ChronoKeeper_ExtractorFailurePreservesChunkForRetry`

Contract:

- If a configured required extractor fails, the chunk is not deleted and is available for retry or outage handling.

Expected initial failure:

- Extraction chain processing is best-effort and chunks are deleted after processing.

Follow-on tests:

- `Unit_ChronoCommon_ExtractionChainAggregatesFailures`
- `Functional_ChronoKeeper_RDMATransferTimeoutReturnsFailure`
- `Functional_ChronoKeeper_OutageBufferFlushesAfterReceiverReturns`
- `Integration_KeeperGrapher_TransferFailureDoesNotLoseEvents`

Risks:

- Retry can duplicate archive chunks.
- Outage buffers can grow without limit.
- Required vs optional extractors may be unclear.

Mitigations:

- Add stable chunk ids.
- Bound outage buffers and return backpressure.
- Mark extractors required/optional in config.

### 7. Atomic HDF5 Archive Publication

First failing test:

- `Unit_ChronoGrapher_HDF5WriterPublishesOnlyCompletedFiles`

Contract:

- HDF5 archive writer writes to a temporary path first, then atomically publishes the completed file. Player ignores temporary files.

Expected initial failure:

- Writer opens final HDF5 path directly.

Follow-on tests:

- `Unit_ChronoGrapher_InvalidArchiveDirFailsStartup`
- `Unit_ChronoPlayer_ArchiveReaderIgnoresTempFiles`
- `Functional_ChronoGrapher_PartialWriteNotReplayable`
- `EndToEnd_ApplicationLogging_NoPartialArchiveReplay`

Risks:

- HDF5 SWMR behavior may interact poorly with rename.
- Existing reader file-name parsing can break.
- Temp files may remain after crash.

Mitigations:

- Use explicit suffix for in-progress files.
- Publish only after close/flush.
- Reader indexes only published names.
- Add cleanup/status for stale temp files.

### 8. Archive Layout And Watermarks

First failing test:

- `Integration_DeployLocal_GeneratesSeparatedOutputRoots`

Contract:

- Local generated configs put Keeper CSV/debug output, Grapher HDF5 archive, and Player scan root in separate directories. Player scans only the HDF5 root.

Expected initial failure:

- Generated config points multiple roles at one output root.

Follow-on tests:

- `Unit_ChronoGrapher_WatermarkAdvancesAfterArchivePublish`
- `Unit_ChronoKeeper_BacklogEvictionRequiresArchiveWatermark`
- `Integration_PlayerArchiveScan_UsesHDF5RootOnly`
- `EndToEnd_ApplicationLogging_ReportsReplayableWatermark`

Risks:

- Existing scripts or docs assume one output directory.
- Watermark can advance before file is safely published.

Mitigations:

- Keep a top-level output dir but create managed subdirs.
- Advance watermark only after successful publish acknowledgment.

### 9. Replay Query Contract

First failing test:

- `Unit_ChronoPlayer_ReplayQueriesUseDistinctIds`

Contract:

- Two concurrent replay queries have distinct ids and responses complete the matching query only.

Expected initial failure:

- Client and player hard-code query id/request id to `1`.

Follow-on tests:

- `Unit_ClientQueryService_ReplayTimeoutReturnsTimeoutStatus`
- `Unit_ClientQueryService_ReplayCompletionWakesWithoutPolling`
- `Integration_ReplayConcurrentRanges_DoNotCollide`
- `Integration_ReplayNoArchive_ReturnsNotReplayableOrNoData`

Risks:

- Wire payload changes can break old clients.
- Timeout cleanup can race with late responses.

Mitigations:

- Include protocol versioning.
- Keep late-response handling idempotent.
- Test response after timeout.

### 10. Replay Planner Across Hot And Archive Tiers

First failing test:

- `Unit_ChronoReplay_PlannerSplitsRangeAtArchiveWatermark`

Contract:

- Given a replay range and an archive watermark, the planner emits one archive subrange and one Keeper hot subrange.

Expected initial failure:

- No replay planner exists; Player reads HDF5 archives only.

Follow-on tests:

- `Unit_ChronoReplay_MergeSortsHotAndArchiveEvents`
- `Unit_ChronoReplay_DedupesHotArchiveOverlap`
- `Unit_ChronoReplay_RejectsUnsupportedConstraint`
- `Integration_ReplayArchiveOnly_ReturnsSortedEvents`
- `Integration_ReplayHotOnly_ReturnsKeeperEvents`
- `Integration_ReplayMixedHotArchive_ReturnsNoDuplicates`

Risks:

- Missing events at hot/archive boundary.
- Duplicate events when archive and Keeper overlap.
- Constraint support can expand beyond schedule.

Mitigations:

- Split by watermark with inclusive/exclusive boundary tests.
- Dedupe by stable event identity.
- Implement minimal constraints and fail closed for others.

### 11. RPC Deadlines And Error Classes

First failing test:

- `Unit_RpcClient_StoppedEndpointReturnsTimeoutStatus`

Contract:

- A configured deadline around a stopped/unresponsive service returns timeout-specific status, not an indefinite block or generic unknown error.

Expected initial failure:

- Client RPCs are blocking calls with generic exception mapping.

Follow-on tests:

- `Integration_RecordStoppedKeeper_ReturnsTimeout`
- `Integration_AcquireStoppedVisor_ReturnsTimeout`
- `Functional_RDMATransferStoppedReceiver_ReturnsUnavailable`
- `Unit_ErrorCodes_AllNewFailuresHaveStringMappings`

Risks:

- Existing tests may rely on long blocking behavior.
- Too-short defaults can make integration tests flaky.

Mitigations:

- Configure deadlines per test.
- Use generous defaults for end-to-end tests.
- Use fake transport wrappers for unit tests.

### 12. Bounded Queues And Backpressure

First failing test:

- `Unit_ChronoCommon_BoundedQueueRejectsWhenFull`

Contract:

- A bounded queue with reject policy returns a backpressure status when full and does not grow unbounded.

Expected initial failure:

- Hot-path queues are raw unbounded deques.

Follow-on tests:

- `Functional_ChronoKeeper_PausedExtractionReturnsBackpressure`
- `Functional_ChronoPlayer_ReplayQueueBackpressure`
- `EndToEnd_ApplicationLogging_BackpressureIsObservable`

Risks:

- Producers may fail where they previously queued forever.
- Backpressure could deadlock if used while holding locks.

Mitigations:

- Document queue policies.
- Avoid blocking while holding global store locks.
- Add metrics and high-water logs.

### 13. Release, Disconnect, And Reconnect Safety

First failing test:

- `Unit_ClientRelease_ServerFailureKeepsLocalHandle`

Contract:

- If server release fails, local story handle remains valid and release can be retried.

Expected initial failure:

- Local reader/writer state is removed before server release succeeds.

Follow-on tests:

- `Unit_ClientDisconnect_ServerFailureRemainsRetryable`
- `Integration_DuplicateConnect_DoesNotClearAcquisitions`
- `Integration_ClientCrashCleanup_ForceReleaseWorks`

Risks:

- Destructor behavior becomes more complex.
- Keeping local handles after failure may surprise callers.

Mitigations:

- Add explicit forced-close path.
- Return structured status indicating retryable cleanup state.

### 14. Python Binding Safety

First failing test:

- `Python_ClientHandleUseAfterReleaseRaisesChronologError`

Contract:

- A Python handle used after release raises a deterministic ChronoLog exception instead of touching deleted C++ memory.

Expected initial failure:

- Python binding returns raw reference to C++ handle.

Follow-on tests:

- `Python_ReplayTimeoutDoesNotBlockOtherPythonThread`
- `Python_PlaybackReturnsStructuredEvent`
- `Python_ImportReportsVersionAndModulePath`

Risks:

- Binding changes can break existing Python examples.
- GIL release around callbacks must be done carefully.

Mitigations:

- Add a safe facade while preserving legacy names.
- Keep Python tests small and deterministic.

### 15. Local Deployment Supervision

First failing test:

- `Integration_DeployLocal_FailedVisorStartFailsDeployment`

Contract:

- If the Visor fails to start or become ready, local deploy exits nonzero and does not launch dependent services.

Expected initial failure:

- Current script uses `nohup` and fixed sleeps.

Follow-on tests:

- `Integration_DeployLocal_StatusShowsManagedPids`
- `Integration_DeployLocal_StopKillsOnlyManagedPids`
- `Integration_DeployLocal_CleanPreservesUnmanagedFiles`

Risks:

- Shell process management is brittle.
- CI environments may differ from developer machines.

Mitigations:

- Keep process ownership in pid files/process groups.
- Test script logic with temporary fake binaries before full service tests.

### 16. Application Logging End-To-End

First failing test:

- `EndToEnd_ApplicationLogging_RecordPlaybackArchiveReplay`

Contract:

- A local deployment accepts structured events from multiple producers, returns the latest event through playback, archives events, replays the requested range, verifies count/order, and shuts down cleanly.

Expected initial failure:

- Playback, hot/archive replay, and stronger archival semantics do not exist yet.

Follow-on tests:

- `EndToEnd_ApplicationLogging_MixedHotArchiveReplayNoDuplicates`
- `EndToEnd_ApplicationLogging_RestartPlayerRebuildsArchiveIndex`
- `EndToEnd_ApplicationLogging_GrapherFailureSurfacesLag`
- `EndToEnd_ApplicationLogging_BoundedReplayTimeout`

Risks:

- End-to-end tests can become slow and flaky.
- Too many assertions in one test obscure failures.

Mitigations:

- Keep one happy-path smoke test.
- Put edge cases in smaller integration tests.
- Use explicit readiness and deadlines.

### 17. Benchmarks And NSF Reporting

First benchmark gate:

- `Benchmark_ChronoLog_LocalAppendPlaybackReplay_Baseline`

Contract:

- Produce repeatable measurements for append throughput, tail playback latency, archive replay throughput, mixed replay latency, archive lag, and replayable lag.

Expected initial failure:

- Current benchmarks do not cover the new playback/replay/watermark contract.

Follow-on benchmark comparisons:

- Kafka local append baseline.
- Kafka latest-event style access baseline.
- Kafka time-range replay baseline where feasible.
- ChronoLog hot playback scenario.
- ChronoLog HDF5 archive replay scenario.

Risks:

- "Beat Kafka" claims can become too broad.
- Benchmarks may measure deployment/config overhead rather than data path.

Mitigations:

- Scope benchmark claims tightly.
- Record hardware, config, payloads, producer counts, and run scripts.
- Report wins, ties, and losses honestly.

## Suggested Implementation Order

1. Clock sync and ChronoTick unit tests.
2. Keeper hot-store unit tests.
3. ATW and collision unit tests.
4. Keeper playback functional/API tests.
5. HDF5 atomic publication tests.
6. Replay query id and timeout tests.
7. Replay planner unit tests.
8. Mixed hot/archive replay integration tests.
9. RPC deadline and backpressure tests.
10. Release/disconnect safety tests.
11. Python safety tests.
12. Local deploy supervision tests.
13. Application logging end-to-end smoke.
14. Benchmark gates and NSF reporting artifacts.

## Done Criteria Per Slice

Each slice is done only when:

- At least one contract test was written before implementation.
- The contract test fails for the expected reason before implementation.
- The feature passes unit tests.
- The feature has at least one negative/error-path test.
- Any new public behavior is covered by integration or end-to-end tests where feasible.
- New error codes are documented and stringified.
- Logs or status output expose the new behavior.
- Existing tests still pass or skipped/manual tests are clearly justified.

