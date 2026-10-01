# ChronoLog Architecture Constitution

Branch: supercomputing-sprint. Release gate: 2026-11-07. Every agent session reads this file before touching the tree. Normative words MUST, MUST NOT and SHOULD are used in the RFC 2119 sense. Every invariant names the test that enforces it. Every number cites a file path or commit, or is marked TBD.

Contract headers referenced below are the ones in `include/chronolog/` and the two proto files in `proto/`. Where this document and a header comment disagree, the header is wrong until an RFC says otherwise, and the PR that notices MUST fix the header.

## 1. Authority and how to change this document

1. Once committed, this file is the sole authority for the ChronoLog rewrite. It does not defer to any session-local document. Where a header comment, a proto comment or a PR description disagrees with it, this file wins and the PR fixes the other.
2. Section 17 is the normative port map. No other port map exists.
3. A change to sections 3 through 10 or to section 15 is a contract change. It MUST go through an RFC (section 16), two reviewer agents, and Kun Feng's QA gate before merge. The durable record of an RFC is the diff to this file plus the PR description.
4. A change to sections 11 through 14 or 16 through 18 is an editorial change. One reviewer agent suffices.
5. An unresolved point is written as a line starting `OPEN:` with both options and a pick. Only the orchestrator resolves it, by replacing the line with the rule and citing the PR. There are none at present.
6. No agent MAY add a line that states a measurement without a path or commit. A claim without a source is marked TBD and is not an invariant.
7. This file lives at the repository root and is the only architecture document in git. Design notes, RFC drafts, reports and plans go to the orchestrator's scratch directory outside git, never to the tree.
8. Test names in this document are normative. Renaming a test named here is a contract change.

## 2. Glossary

- **Chronicle**: a namespace of stories. **Story**: one append-only log identified by `story_id`.
- **story_id**: uint64 assigned by Catalog. Never derived from names. Legacy derived it as CityHash64(chronicle+story), which collides on ("a","bc") versus ("ab","c") (`src/chrono-common/include/ArchiveManifest.h`, origin/archive-file-manifest). That derivation is forbidden.
- **writer_id**: uint64 assigned by Catalog at acquire. **incarnation**: uint64 persisted per writer identity, strictly increasing on every re-acquire. **sequence**: uint64 per (writer_id, incarnation), starts at 1, gapless.
- **EventId**: (story_id, writer_id, incarnation, sequence). The identity of an event. Equality of EventId is equality of events. `include/chronolog/types.h`.
- **HLC**: hybrid logical clock {int64 physical_ns; uint32 logical}. Keeper-assigned on accept.
- **causal_floor**: the maximum HLC a client has observed from any append result or read. Sent on every append item.
- **physical time**: the writer's CLOCK_REALTIME reading in ns, with an optional uncertainty bound and a clock status.
- **Durability**: ACCEPTED (Keeper RAM) or DURABLE (fsync'd Keeper WAL). Only DURABLE is an ack. UNSPECIFIED in a request means DURABLE and is never an achieved level.
- **Visor**: the process hosting Catalog and Cluster. **Keeper**: ingestion and WAL, hosts Journal. **Grapher**: tiering and archive, hosts TierStore publish. **Player**: hosts Replay.
- **Epoch**: per-story ownership version. A Route is valid for exactly one epoch.
- **Route**: {epoch, keepers, grapher, player} for a story. **KeeperRef**: {process_id, endpoint}, the one Keeper identity used by Route.keepers and Acquisition.assigned_keeper; internal subscriptions use process_id alone.
- **Pending-fsync HLC**: per writer, the HLCs assigned to DURABLE events whose fsync has not completed. Read only under that writer's lock.
- **Chunk**: a half-open HLC window [start, end) of one story's events, the unit of Keeper to Grapher transfer and of archive publication. Identified by a string `chunk_id` unique per Keeper.
- **Revision**: a monotonically increasing number on every AcquisitionUpdate, a persisted Catalog counter in SQLite that is never reused across Visor restarts. Keepers report `applied_revision` in Heartbeat. **fenced**: the per-item bool a Release returns, true iff the assigned Keeper applied the release revision within the fence timeout.
- **Receipt**: a Grapher-assigned number for one delivered chunk, scoped to a Grapher instance string. Delivery is not persistence.
- **W (persisted watermark)**: per-story end of the longest contiguous run of persisted windows from the story's earliest recorded window. Monotonic within a live story generation; a tombstone ends the generation.
- **E**: the Keeper's eviction boundary. Invariant E <= W.
- **Salvage chunk**: a chunk written outside the contiguous timeline. Watermark-exempt. Readable, never advances W.
- **Manifest**: the archive's append-only record of published files, one log and one snapshot per Grapher. `ManifestRecord.manifest_writer` names the Grapher instance that owns a log; it is distinct from the event `writer_id`.
- **Frontier (sealed, exclusive)**: for writer w assigned to Keeper k, an HLC F with the sealed-prefix property: every event of w with hlc < F is visible to reads and no event of w with hlc < F will ever be ingested. That property is the whole definition. With mixed durability a pending DURABLE event caps F below ACCEPTED events that are already visible. All writers on one Keeper share that Keeper's sealed frontier.
- **Completion**: the trailing message of a Replay.Read stream stating complete, frontier and laggards.
- **Gateway**: the out-of-core process that does authn, ACLs, redaction and audit. Not part of this repository's core.
- **Contract**: one of the six pure-virtual C++ interfaces in `include/chronolog/`.
- **Walking skeleton**: Visor Catalog plus Keeper Journal in RAM plus one client, composed end to end before any domain port.

## 3. Data model and the three time fields

Every event carries exactly three time-related fields. None substitutes for another.

| Field | Who assigns | Type | Use |
| --- | --- | --- | --- |
| physical | writer, at creation | TimeReading{physical_ns int64, uncertainty_ns optional uint64, status} | Range queries on wall time, auditing, OTel alignment |
| hlc | Keeper, on accept | Hlc{physical_ns int64, logical uint32} | Total order and causality |
| identity | Catalog (ids) and writer (sequence) | EventId | Deduplication, per-writer order, retry |

Every stored and streamed Event carries `durability`, the achieved level (ACCEPTED or DURABLE), so readers can apply I6.8. Envelope fields, all optional: `content_type` string; `payload` bytes, max size configurable, default 1 MiB, enforced at the Keeper; `trace_id` bytes(16) and `span_id` bytes(8) carrying W3C trace context; `attributes` map<string,string> for OTel GenAI keys (gen_ai.agent.id, gen_ai.conversation.id, gen_ai.tool.call.id, gen_ai.operation.name), MCP request ids, and A2A task_id, context_id, message_id. Key names follow the OTel GenAI semantic conventions (gen-ai-agent-spans), the A2A specification proto and the newest dated MCP schema, never the 2024-11-05 one.

Invariants:
- I3.1 An event MUST carry a complete EventId (all four fields non-zero) before the Keeper accepts it. story_id and epoch come from the request, writer_id, incarnation and sequence from the item (W10.13). Gate: `JournalContract.RejectsIncompleteEventId (to add)`.
- I3.2 A payload above the configured maximum MUST be rejected per item with INVALID_ARGUMENT in the item status, never truncated. A trace_id not exactly 16 bytes or a span_id not exactly 8 bytes, when present, MUST be rejected the same way. Gate: `JournalContract.PayloadAndTraceContextValidation`.
- I3.3 The Keeper MUST NOT modify physical time. It MUST assign hlc. Gate: `JournalContract.EnvelopeAndPhysicalReadingRoundTrip`.
- I3.4 Story and chronicle names map to ids in Catalog only. No component MAY compute an id from a name. Gate: `MetadataStoreContract.AssignedIdsDoNotAliasConcatenatedNames`.
- I3.5 Tombstones are identity tombstones: a destroyed story_id never returns and Catalog keeps its tombstone forever; the name may be reused by a new story with a new story_id. Gate: `MetadataStoreContract.StoryTombstonePermanence`.
- I3.6 Destroying a chronicle tombstones every story in it. Destroy of a story or chronicle MUST be refused with FAILED_PRECONDITION while any acquisition on it is active. Gate: `MetadataStoreContract.ChronicleTombstonePermanence`, `MetadataStoreContract.DestroyRefusedWhileAcquired (to add)`.
- I3.7 Release fences the incarnation: once (writer_id, incarnation) is released, the Keeper MUST reject every later append carrying it with FAILED_PRECONDITION, and a release carrying an older incarnation MUST NOT release a newer one. Gate: `MetadataStoreContract.OldReleaseCannotReleaseNewIncarnation`, `JournalContract.ReleasedIncarnationCannotAppend (to add)`.

## 4. Ownership epochs

Every story has one owning Route per epoch. Every AppendItem carries (story_id via EventId, epoch).

- I4.1 `Membership::validateEpoch(story_id, epoch)` MUST reject a stale epoch. A stale epoch is never a gRPC error: Append returns gRPC OK, the affected items carry FAILED_PRECONDITION in their ItemStatus plus `current_route`, and because a batch is single-story (W10.13) a stale request-level epoch rejects every item and the AppendResponse also carries a top-level `current_route`. No trailing-metadata encoding. Gate: `MembershipContract.StaleEpochRejection`; `JournalContract.StaleEpochReturnsCurrentRoute`.
- I4.2 Epochs MUST strictly increase per story. `MetadataStore::compareAndSetEpoch` is the only mutator and MUST reject a desired epoch that is not greater than the current one. Gate: `MetadataStoreContract.EpochCompareAndSet`.
- I4.3 Until PR 8, epochs are static: every story is epoch 1 and routes are fixed from configuration. Implementations MUST still carry and validate the field so clients are correct from PR 1. Gate: `MembershipContract.StaticEpochIsOneAndValidated (to add)`.
- I4.4 A client receiving a FAILED_PRECONDITION item MUST adopt the returned Route and retry with the same EventIds. Retries are idempotent (section 5). Gate: `tests/contract/client/` test `ClientRetriesOnStaleEpochWithSameEventIds`.
- I4.5 Cluster.WatchRoutes MUST send a full snapshot on subscribe and on reconnect. A Keeper MUST NOT serve a story before it has a Route for it. Gate: `MembershipContract.UnknownStoryCannotValidate`.

- I4.6 A Route change MUST NOT invalidate an in-flight Keeper to Grapher chunk stream; the stream completes under the old epoch. Chunks are immutable once sealed and the receipt protocol tolerates resends. Gate: `ArchiveTransferTest.StreamCompletesAcrossEpochChange (to add, PR 8)`.

## 5. Ack semantics

`enum Durability { DURABILITY_UNSPECIFIED = 0; ACCEPTED = 1; DURABLE = 2; }` in `proto/chronolog/v1/chronolog.proto`. UNSPECIFIED means DURABLE in a request and MUST never appear as an achieved success level. Default DURABLE.

- ACCEPTED: the event is in Keeper RAM and ordered. It is lost on Keeper crash.
- DURABLE: the event is fsync'd in the Keeper WAL with group commit. It survives Keeper crash.
- Only DURABLE is called an ack in any documentation, log line, metric or SDK name.

Invariants:
- I5.1 A server that cannot provide the requested level MUST reject that item with UNIMPLEMENTED (level not built) or UNAVAILABLE (level built but failing) in the ItemStatus and `achieved_durability = DURABILITY_UNSPECIFIED`. It MUST NOT return ACCEPTED when DURABLE was requested. Status codes are fixed by this file and asserted exactly by headers and contract tests: unbuilt durability UNIMPLEMENTED, built but failing UNAVAILABLE, sequence gap FAILED_PRECONDITION with the expected sequence, payload or trace-context violation INVALID_ARGUMENT, stale epoch or released incarnation FAILED_PRECONDITION. Gate: `JournalContract.NoSilentDurabilityDowngrade`.
- I5.2 Before the WAL lands (PR 5), every DURABLE append MUST fail per I5.1 with UNIMPLEMENTED. The walking skeleton MUST request ACCEPTED explicitly. Gate: `JournalContract.AcceptedIsExplicitRamReceipt` instantiated for the RAM Journal.
- I5.3 AppendResult per item MUST carry status, achieved durability, assigned hlc, EventId. Gate: `JournalContract.AppendResultCarriesAllFourFields (to add)`.
- I5.4 A duplicate EventId MUST return the original result (same hlc, achieved durability equal or higher) and MUST NOT store a second copy. Gate: `JournalContract.IdempotentRetryReturnsOriginalResult`.
- I5.5 A gap in sequence for (writer_id, incarnation) MUST be rejected per item with FAILED_PRECONDITION and the expected next sequence in the item message. Later items of the same writer in that batch MUST also be rejected. Gate: `JournalContract.GaplessSequenceRejection`.
- I5.6 DURABLE MUST mean fsync returned success, not write() returned. An fsync failure MUST fail every item in the group with UNAVAILABLE and MUST NOT report DURABLE for any of them. Gate: `JournalContract.FsyncFailureFailsTheGroup (to add)` (fault-injected file sink). Crash gate: `tests/integration/keeper_wal_restart_test.sh` (TBD until PR 5).
- I5.7 Legacy RAM retention (KeeperChunkRetentionStore) cannot implement DURABLE and MUST NOT be labelled as such.
- I5.8 AppendStream cancellation MUST NOT roll back accepted items. Gate: `JournalAdapterTest.CancelledStreamKeepsAcceptedItems (to add)`.
- I5.9 A batch MUST be processed in item order per writer; an item's result position equals its request position. Gate: `JournalContract.PerItemFailureDoesNotEraseSuccessfulBatchItems`.

- I5.10 The WAL group-commit window is a fixed timer exposed as a config knob, initial value 1 ms, set in PR 5. No latency claim is made until measured (section 18).

## 6. Completeness semantics

`Replay.Read(story, [start, end))` on hlc or physical time returns EventBatch messages and ends with exactly one Completion: `complete bool`, `frontier hlc`, `repeated laggards {writer_id, incarnation, frontier}`. Then EOF.

Completion also carries `IncompleteReason reason` with values LAGGING_WRITERS, PHYSICAL_AXIS_UNBOUNDED, SOURCE_FAILED, TRUNCATED. It is set whenever `complete=false`.

Definition: the read is complete iff every Keeper in the story's Route answered and its sealed frontier F_k >= end, on the HLC axis. Equality is sufficient because the frontier is exclusive. The registered-writer view from WatchAcquisitions is used only to name laggards (writers assigned to a Keeper that failed or lags); a stale acquisition view therefore cannot produce a false complete. The Keeper obtains F by ticking its HLC (future assignments are > F) under the rules in I6.11; for DURABLE data F is capped at the minimum pending-fsync HLC. Idle registered writers share their Keeper's frontier and do not block completeness. A writer is registered from acquire until release or incarnation supersession. Completeness in v1 is defined on the HLC axis only. An HLC frontier does not bound a writer's future backdated CLOCK_REALTIME stamps, so a physical-axis Read MUST return `complete=false` with reason PHYSICAL_AXIS_UNBOUNDED until PR 6 lands an acceptance policy that bounds backdating.

Invariants:
- I6.1 An HLC Read with some registered writer's sealed frontier below end MUST set `complete=false`, reason LAGGING_WRITERS, and list that writer in laggards. A frontier equal to end is sufficient. Laggards are the writers whose Keeper did not answer or whose sealed frontier is below end. Gate: `ReplayContract.CompletionNamesLaggardsAndEqualityIsSufficient`, `ReplayContract.CompleteWhenEveryKeeperSealReachesEnd`, `ReplayContract.IdleRegisteredWriterDoesNotBlockCompleteness`, `ReplayContract.UnknownWriterCoveredByItsKeeperSeal`.
- I6.2 A Read MUST NOT drop events that are below the archive boundary but not yet confirmed persisted. Legacy guard: `tests/unit/chrono-player/chrono_player_hot_range_split_test.cpp::UnacknowledgedEventsBelowTheBoundaryAreKept`. Contract gate: `ReplayContract.UnconfirmedHotEventsBelowBoundarySurvive`.
- I6.3 A Keeper that fails to answer MUST make the reply incomplete with reason SOURCE_FAILED; a truncated answer MUST make it incomplete with reason TRUNCATED. Legacy guards: `chrono_player_hot_range_split_test.cpp::AKeeperThatDidNotAnswerWidensTheArchiveReadAndMarksTheReplyIncomplete`, `::ATruncatedAnswerMarksTheReplyIncompleteWithoutWideningTheRead`. Contract gate: `ReplayContract.FailedSourcePreventsCompleteness`, `ReplayContract.TruncatedSourcePreventsCompleteness`.
- I6.4 Events MUST be deduplicated by EventId across hot and archive sources. Legacy guards: `chrono_player_replay_event_merge_test.cpp::EventFromTheArchiveAndAKeeperIsReturnedOnce`, `::EventsDifferingOnlyInClientIdAreDifferentEvents`. Contract gate: `ReplayContract.TotalOrderAndEventIdentityDeduplication`.
- I6.5 `Replay.Tail` MUST NOT claim completeness. Its Completion on orderly termination MUST have `complete=false`. Gate: `ReplayContract.TailNeverClaimsCompleteness`.
- I6.6 `Journal::frontier(story)` MUST return the Keeper's sealed frontier for every writer registered on that Keeper, including writers with zero events, and that value MUST satisfy the sealed-prefix property and nothing more; in particular it MUST NOT exceed the minimum pending-fsync HLC. When nothing is pending it exceeds every assignment. Gate: `JournalContract.HalfOpenRangeAndFrontierIncludesRegisteredWriter`, `JournalContract.SealedFrontierExceedsEveryAssignmentWhenNothingPending`, `JournalContract.DurableInvisibleUntilFsyncAndSealDoesNotPassPendingHlc (PR 5)`, `JournalContract.IdleRegisteredWriterDoesNotBlockCompleteness`.
- I6.7 A Read on a tombstoned story MUST fail whole-request with FAILED_PRECONDITION. Gate: `ReplayContract.ReadOnTombstonedStoryFails (to add)`.
- I6.8 `complete=true` MUST imply that every event in the range that any writer will ever produce is in the stream. A later Read of the same range MUST return the same event set for DURABLE events; ACCEPTED events may vanish on Keeper crash and the invariant says so. Visibility: an ACCEPTED event is visible once accepted, a DURABLE event only after its fsync. Gate: `ReplayContract.CompleteReadIsStableForDurableEvents`.
- I6.9 Tail resumes exclusively after `position` (hlc, EventId); the event at `position` MUST NOT be re-sent. Gate: `ReplayContract.TailResumesExclusivelyAfterPosition`.
- I6.10 A physical-axis Read in v1 MUST return `complete=false` with reason PHYSICAL_AXIS_UNBOUNDED even when every frontier is past the range. Gate: `ReplayContract.PhysicalAxisAlwaysIncomplete`. PR 6 replaces this test with the acceptance-policy gate.
- I6.11 Frontier ordering rules, all MUST. (a) Archive.FetchHot ticks F before it scans events, never after, so every event with hlc < F accepted during the scan is in the reply. (b) HLC assignment and insertion are atomic under the writer's lock, and the frontier tick happens before any per-writer lock is taken for the scan. (c) Completeness is decided over every Keeper in the Route; the Player maps registered writers to `assigned_keeper` from WatchAcquisitions only to name laggards, and a Keeper that did not answer makes the read SOURCE_FAILED with its writers listed. (d) From PR 8, an epoch change carries a floor equal to the maximum last reported frontier of every Keeper of the previous epoch; every Keeper of the new epoch MUST `observe()` that floor before accepting any writer, and a reassigned writer additionally carries its old Keeper's `last_reported_frontier`. A writer's new incarnation is never admitted on a different Keeper until its old incarnation is fenced on the old Keeper. Under static epochs assigned_keeper is a function of writer_id, so both incarnations land on one Keeper and I7.4 rejects late old-incarnation appends. (e) Pending-fsync state is tracked per writer and registered in the same writer-lock critical section as HLC assignment. The scan ticks h_t first, then for each writer takes that writer's lock, scans its visible events and reads that writer's minimum pending-fsync HLC while still holding the lock. F = min(h_t, min over writers of those per-writer minima). Two conditions make this sound and are part of the rule: fsync completion makes an event visible and clears its pending entry under the same writer lock, so a scan never observes an event that is neither visible nor pending; and writer state is created under the story lock before the writer's first HLC assignment, and the scan iterates the writer set as it is after the tick, so every writer assigned before the tick is scanned. A global pending set sampled outside the writer locks is unsound whether sampled before the scan (an event assigned before the tick but registered after the sample) or after it (an fsync completing after the scan drops an unreturned event from the cap). (f) FetchHotTrailer carries the Keeper's current epoch; the Player MUST treat a trailer epoch different from its Route epoch as SOURCE_FAILED. Gates: `ArchiveTransferTest.FetchHotTicksFrontierBeforeScan (to add)`, `ArchiveTransferTest.FrontierTickOrderedBeforeInsert (to add, TSAN)`, `ReplayContract.UnknownWriterCoveredByItsKeeperSeal`, `JournalContract.PendingFsyncRegisteredWithAssignment (to add, TSAN, PR 5)`, `JournalContract.FsyncCompletionFlipsVisibilityUnderWriterLock (to add, TSAN, PR 5)`, `JournalContract.WriterCreatedBeforeFirstAssignmentIsScanned (to add)`, `JournalContract.ReassignedWriterObservesOldFrontier (to add, PR 8)`, `ReplayContract.TrailerEpochMismatchIsSourceFailed (to add, PR 8)`, `AcquisitionWatcherTest.NewOwnerAdmittedOnlyAfterOldOwnerFenced (to add, PR 8)`.

## 7. Ordering

Total order for replay: (hlc, writer_id, incarnation, sequence), lexicographic. Implemented once as `ReplayLess` in `include/chronolog/types.h`. Per-writer order is sequence order and is never violated.

- I7.1 Replay output MUST be sorted by the total order within one stream. Gate: `ReplayContract.TotalOrderAndEventIdentityDeduplication`.
- I7.2 For a fixed (writer_id, incarnation), hlc MUST be non-decreasing in sequence. The Keeper enforces this at accept because it assigns hlc in arrival order and rejects gaps. Gate: `JournalContract.PerWriterOrderSurvivesBackwardPhysicalStep`.
- I7.3 Read-then-write causality: if client A observes event X (hlc h) and then appends Y with causal_floor >= h, then hlc(Y) > h. Gate: `JournalContract.KeeperAssignsHlcAboveCausalFloor`.
- I7.4 A new incarnation of a writer MUST sort after every event of its previous incarnation. The Keeper MUST reject an append whose incarnation is below the highest it has seen for that writer. Gate: `JournalContract.OlderIncarnationIsRejected (to add)`.
- I7.5 Two Keepers serving one story (a Route lists several keepers for partitioning) MUST NOT both assign hlc for the same writer. Writer to Keeper affinity is `Acquisition.assigned_keeper`, stable per (writer_id, epoch); a Keeper MUST reject an append from a writer not assigned to it with FAILED_PRECONDITION and current_route. Gate: `MetadataStoreContract.AssignedKeeperStableWithinWriterEpoch`, `JournalContract.UnassignedKeeperRejectsWriter (to add)`.
- I7.6 Legacy timestamp ordering guard retained for the Clock port: `tests/unit/chronolog-client/chronolog_client_chrono_clock_test.cpp::StrictlyIncreasesEvenWhenRawStepsBackward` (origin/visor-clock-exchange).

## 8. Clock contract

`Clock::now() -> TimeReading{physical_ns, uncertainty_ns optional, status}` with status Synced, Unsynced or Unavailable (`include/chronolog/clock.h`).

- v1 bound: uncertainty_ns = root_delay/2 + root_dispersion + drift * age, read from chrony or the kernel. The drift coefficient source is TBD (chronyc tracking or adjtimex); until it is implemented the status is Unsynced whenever the source is unavailable.
- Unsynced and Unavailable MUST carry no finite bound: `uncertainty_ns` is absent. An absent bound means unknown, never zero.
- `Clock::tick() -> Hlc` for a local event, `Clock::observe(Hlc) -> Hlc` merges a remote HLC with the standard HLC rule, `Clock::uncertainty()` returns the last bound.
- No ML in v1. A learned predictor MUST NOT shrink the bound until residual coverage is validated.
- `chronolog.internal.v1` Cluster.ReadClock returns the Visor's monotonic authority tick for a Cristian round-trip estimate (offset = authority_tick_ns + RTT/2 - t1). That tick is kept separate from CLOCK_REALTIME and from the chrony bound. RTT uncertainty MUST NEVER replace physical uncertainty, and ReadClock MUST NOT be used by the Keeper to assign hlc. It is not in `chronolog.v1`; it can be promoted later additively. Writers without chrony report Unsynced physical time.
- Legacy Cristian exchange with the Visor (`client/cpp/include/ChronoClock.h`, origin/visor-clock-exchange) reported uncertainty = RTT/2. That exchange is replaced by the chrony-backed Clock; the monotonic clamp and the injected TimeSource test seam are kept.

Invariants:
- I8.1 HLC MUST strictly increase per Keeper even when physical time steps backward or the source becomes Unavailable. Gate: `ClockContract.HlcMonotonicUnderBackwardPhysicalStep`, `ClockContract.UnavailableSourceKeepsHlcAdvancing`.
- I8.2 `observe(h)` MUST return an Hlc > h and > the last tick. Gate: `ClockContract.ObservePreservesReadThenWriteCausality`.
- I8.3 A Synced reading MUST have a finite bound; an Unsynced or Unavailable reading MUST NOT. Gate: `ClockContract.UnsyncedAndUnavailableHaveNoFiniteBound`.
- I8.4 The HLC logical counter MUST reset to 0 when physical advances and MUST increment otherwise. Gate: `ClockContract.StandardHlcLogicalTick`.
- I8.5 The Keeper MUST assign hlc = max(local physical, last assigned, client causal_floor) with tick. Gate: `JournalContract.KeeperAssignsHlcAboveCausalFloor`.
- I8.6 A causal_floor far in the future MUST NOT be accepted blindly. The Keeper MUST reject an item whose causal_floor.physical_ns exceeds local physical by more than a configured skew limit (default TBD) with INVALID_ARGUMENT. Gate: `JournalContract.AbsurdCausalFloorIsRejected (to add)`.
- End-to-end gate kept after the port: `tests/end-to-end/clock-skew/clock_skew_harness.sh` (origin/visor-clock-exchange), asserting that the absolute error of the measured offset is at most the reported uncertainty plus 2 ms slack (`SLACK_NS=2000000` in that file).

- I8.7 `Hlc.physical_ns` is CLOCK_REALTIME on the Keeper. HLC tolerates steps by construction (I8.1) and this keeps hlc comparable to the writer's physical field. Gate: `ClockContract.HlcMonotonicUnderBackwardPhysicalStep`.

## 9. The six interfaces

All live in `include/chronolog/`, are pure virtual, return `absl::Status` or `absl::StatusOr<T>`, and contain no gRPC or protobuf types. Shared value types live in `types.h`: EventId, Hlc, TimeReading, Durability, Route, Epoch, Envelope, Event, AppendItem, AppendResult, Range, Frontier, Completion, Chunk, ManifestRecord, ChunkReceipt, WatermarkReport, ReplayBatch. Thread-safety stated here and in the header comments is normative.

**Clock** (`clock.h`): now() -> StatusOr<TimeReading>, tick() -> Hlc, observe(Hlc) -> Hlc, uncertainty(). tick() and observe() are infallible: loss of the physical source sets status Unavailable on now() and HLC continues from last assigned plus logical ticks. All methods MUST be safe to call concurrently from any thread; tick and observe serialize on one atomic or mutex; now() MUST NOT block on I/O, so the chrony refresh runs on a background thread owned by the implementation. Invariants I8.1 through I8.4. Suite: `ClockContract`.

**MetadataStore** (`metadata_store.h`): createChronicle, getChronicle, listChronicles, destroyChronicle, createStory, getStory, listStories, destroyStory, acquire(story, writer_identity) -> Acquisition{story_id, writer_id, incarnation, route, assigned_keeper}, release, compareAndSetEpoch. `assigned_keeper` is a KeeperRef{process_id, endpoint}, the single Keeper for this writer in the current epoch, stable per (writer_id, epoch); AcquireResponse carries no epoch field of its own, route.epoch is the only epoch in that response; it enforces I7.5 while Route still lists every keeper. All methods thread-safe and linearizable per key. Persistence for PR 2 is SQLite (vcpkg port sqlite3) in WAL journal mode with synchronous=FULL; acquire and incarnation bumps MUST commit before the RPC returns. PR 8 replicates the same operations through NuRaft behind the same contract. Invariants I3.4 through I3.6, I4.2, plus `MetadataStoreContract.PersistedIncarnationStrictlyIncreases`, `MetadataStoreContract.StoryTombstonePermanence`, `MetadataStoreContract.WriterIdIsStableAcrossReacquire (to add)`. Suite: `MetadataStoreContract`.

**Membership** (`membership.h`): route(story_id) -> Route, validateEpoch(story_id, epoch), registerProcess(Process{id, instance, endpoint, role}), heartbeat(id, instance). route() returns a snapshot copy taken under a bounded shared-lock section; validateEpoch() takes at most a shared lock; registration may take an exclusive lock. Invariants I4.1, I4.3, I4.5, I7.5, plus `MembershipContract.RegisterHeartbeatAndRestartFencing`. Suite: `MembershipContract`.

**Journal** (`journal.h`): append(AppendBatch{story_id, epoch, items}, durability) -> vector<AppendResult> (single-story batches, W10.13); read(story, range) -> vector<Event>; frontier(story) -> vector<Frontier>; keeperFrontier(story) -> Hlc, the Keeper-level seal even when no writer is registered, which FetchHotTrailer carries as `sealed_frontier`. append() MUST be callable concurrently for different stories; for one writer, acceptance and hlc assignment MUST be atomic. read() holds each writer lock only for that writer's scan slice and never across the whole story. Invariants I3.1 through I3.3, I5.1 through I5.9, I6.6, I7.2 through I7.4, I8.5, I8.6, I13.8. Suite: `JournalContract`.

**TierStore** (`tier_store.h`): publish(Chunk) -> ManifestRecord; read(story, range); manifest(story) -> vector<ManifestRecord>; contiguousWatermark(story) -> Hlc. publish() MUST be thread-safe across stories and across concurrent streams of one story (legacy `ArchiveManifest::append` is mutex-serialized for this reason). Visibility is API visibility: a reader sees a chunk only through its manifest record, never by scanning files; publish() is atomic to readers in that sense. read() MUST be safe concurrently with publish(). Invariants I13.1 through I13.7, I12.2 through I12.4. Suite: `TierStoreContract`.

**Replay** (`replay.h`): read(story, range) and tail(story, position) each return a `ReplayStream` with next() -> optional<ReplayBatch> (nullopt is EOF) and cancel(). A stream has exactly one consumer; distinct streams are independent; cancel() is thread-safe, idempotent and unblocks a pending next(); destroying a stream cancels and joins owned work. The implementation MUST NOT invoke the consumer under any internal lock. Invariants I6.1 through I6.5, I6.7 through I6.9, I7.1, I12.6. Suite: `ReplayContract`.

Every suite MUST be declared with `GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST` so PR 1 is green with no implementations. Every component port MUST instantiate the suite for its implementation. A suite instantiation is a `INSTANTIATE_TEST_SUITE_P` in the component's own test directory, never inside `tests/contract/`.

## 10. Wire API rules

Public package `chronolog.v1`, file `proto/chronolog/v1/chronolog.proto`, services Catalog, Journal (Append unary, AppendStream bidi), Replay (Read, Tail server streams). Internal package `chronolog.internal.v1`, file `proto/chronolog/internal/v1/internal.proto`, services Archive (TransferChunk client stream, WatchWatermarks server stream) and Cluster (Register, Heartbeat, WatchRoutes, WatchAcquisitions, ReadClock for skew observability among cluster processes). Archive.FetchHot(FetchHotRequest{story_id, range, max_events}) returns a stream of event batches then one FetchHotTrailer{sealed_frontier, epoch, frontiers, truncated}, served by the Keeper for the Player. Cluster.WatchAcquisitions(keeper id) streams AcquisitionUpdate{story_id, writer_id, incarnation, assigned_keeper, state ACQUIRED or RELEASED} so Keepers learn registered writers and fence released incarnations (I3.7). The internal package is not frozen; additions need no RFC, renames and removals do.

- W10.1 `chronolog.v1` is additive-only after PR 1. A breaking change MUST create `chronolog.v2`. Field numbers MUST never be reused; removed fields MUST be `reserved` by number and name. Gate: `tests/contract/proto/proto_compat_test` runs `buf breaking` against the PR 1 descriptor set checked in under `proto/baseline/` (TBD until PR 1 lands).
- W10.2 `chronolog.internal.v1` is versioned with the server release. It MUST NOT be exposed by the gateway and MUST NOT be generated into SDKs. Gate: SDK build scripts MUST NOT reference `proto/chronolog/internal/`; CI greps for it.
- W10.3 Per-item results live in response messages as ItemStatus{code, message} using google.rpc.Code integers. gRPC status codes are used only for whole-request failures: UNAVAILABLE, INVALID_ARGUMENT (malformed request, unset oneof), UNIMPLEMENTED, FAILED_PRECONDITION (tombstoned story, unknown story). A stale epoch is never a gRPC error: the RPC returns OK and the rejection is per item with current_route (I4.1). Gate: `JournalContract.StaleEpochReturnsCurrentRoute`, `ReplayContract.ReadOnTombstonedStoryFails (to add)`.
- W10.4 Archive.TransferChunk is a client stream of ChunkFrame{identity, offset, total_bytes, checksum, checksum_algorithm, data, final}. Every frame repeats identity, total_bytes, checksum and checksum_algorithm (CRC32C default via absl/crc/crc32c.h, SHA256 optional); offsets are gapless; the checksum is over the whole concatenated data; `final` MUST coincide with byte count reached and stream EOF. The response is one ChunkReceipt{status, chunk_id, bytes, grapher_instance, receipt}. A partial stream, offset gap, byte mismatch or checksum mismatch MUST produce a receipt with a non-OK status and MUST NOT be treated as delivered. Legacy shape: `src/chrono-common/include/ChunkReceipt.h` (origin/700-watermark-feedback). Gate: `ArchiveTransferTest.PartialStreamYieldsNoReceipt (to add)`, `ArchiveTransferTest.ChecksumMismatchYieldsNoReceipt (to add)`.
- W10.5 WatermarkReport{story_id, watermark, grapher_instance, highest_receipt, pending_receipts sorted, dropped}. Every receipt from grapher_instance up to highest_receipt is written except those in pending_receipts. `dropped=true` replaces the legacy `kStoryDroppedWatermark = UINT64_MAX` sentinel and means the story is gone and every chunk for it MUST be freed. Legacy semantics: `StoryWatermarkReport` in ChunkReceipt.h. Gate: `JournalContract.DroppedStoryReportFreesRetainedChunks (to add)`.
- W10.6 Cluster.WatchRoutes sends full snapshots on subscribe and reconnect (I4.5). Heartbeat carries (process_id, instance, applied_revision, repeated per-story frontiers); a new instance replaces the old registration. A Keeper MUST send a Heartbeat immediately after applying a RELEASED update so Release latency is not bounded below by the heartbeat interval.
- W10.7 Contract headers MUST NOT include generated protobuf or gRPC headers. Conversion lives in `src/chrono-<service>/adapter/`. Gate: `tests/contract/include_guard_test.sh` greps `include/chronolog/` for `.pb.h` and `grpcpp`.
- W10.8 Every AppendRequest MUST carry `story_id` and `epoch`; a zero epoch is whole-request INVALID_ARGUMENT. Read and Tail do not carry an epoch because Players serve any epoch's data. Gate: `JournalAdapterTest.MissingEpochIsInvalidArgument (to add)`.
- W10.9 All timestamps on the wire are int64 nanoseconds, authority_tick_ns included. No Duration or Timestamp well-known types in `chronolog.v1`, so SDKs in every language compare plain integers.
- W10.10 A oneof that is unset MUST fail whole-request with INVALID_ARGUMENT. Gate: `ReplayAdapterTest.UnsetRangeIsInvalidArgument (to add)` at the adapter level.
- W10.11 Responses echo `batch_id`. SDKs correlate on it, never on arrival order, for AppendStream.
- W10.12 Release returns ReleaseResponse{status, fenced}. Catalog commits the release, then waits up to `release_fence_timeout_ms` for the assigned Keeper's Heartbeat `applied_revision` to reach the release's revision. fenced=false means the release is committed but the Keeper had not confirmed; appends that Keeper accepted before fencing stay valid and sort before any newer incarnation. No claim of linearizable release is made. Release is idempotent by (story_id, writer_id, incarnation): a retry after a lost response returns the committed revision and the current fenced state, which is also how a client polls fence state. Gate: `MetadataStoreContract.ReleaseReportsFenceState`, `CatalogAdapterTest.DroppedReleaseRetryReturnsCommittedState (to add)`, adapter `CatalogAdapterTest.ReleaseWaitsForAppliedRevision (to add)`.
- W10.13 Append batches are single-story. AppendRequest carries story_id, epoch, durability, batch_id and items; an item carries writer_id, incarnation, sequence, physical, causal_floor and envelope; a result carries the full EventId. A batch mixing stories is impossible by construction and the top-level current_route is unambiguous. Gate: `JournalContract.PerItemFailureDoesNotEraseSuccessfulBatchItems` on the new shape.
- W10.14 AcquisitionUpdate and ReleaseResponse carry `revision`, a persisted Catalog counter never reused across Visor restarts, and the proto comments say so; HeartbeatRequest carries `applied_revision`. There is one global Catalog revision counter. A Keeper's applied_revision is the highest R such that every update relevant to that Keeper with revision <= R is applied; irrelevant revisions are skipped implicitly, and applied_revision is scoped to the reporting process instance. WatchAcquisitions on subscribe or reconnect sends a full snapshot of the acquisitions assigned to that Keeper as of revision R, then deltas above R; a (re)started Keeper applies the snapshot, restoring every fence, before admitting any append. Gates (acquisition adapters, to add): `AcquisitionWatcherTest.RevisionGapsAreSkipped`, `AcquisitionWatcherTest.RestartRestoresFencesBeforeAdmission`, `AcquisitionWatcherTest.AppliedRevisionIsMonotonic`.
- W10.15 CLOCK_STATUS_UNSPECIFIED on the wire maps to Unavailable in every adapter, never to Synced; the proto comment states it. Gate: `JournalAdapterTest.UnspecifiedClockStatusIsUnavailable (to add)`.

## 11. Module and dependency rules

Directory ownership after the hard cut. Directory names are kept; ports happen in place.

| Directory | Owner role | May include |
| --- | --- | --- |
| `include/chronolog/` | architect (contracts) | absl, std only |
| `proto/` | architect | `chronolog/v1` from internal only |
| `src/chrono-common/` | domain port | `include/chronolog/`, absl, std, HDF5 (tier only) |
| `src/chrono-visor/` | Visor port | `src/chrono-common/`, contracts, generated proto, gRPC |
| `src/chrono-keeper/` | Keeper port | same as visor |
| `src/chrono-grapher/` | Grapher port | same as visor |
| `src/chrono-player/` | Player port | same as visor |
| `client/cpp/` | SDK port | contracts (Clock, types), generated `chronolog.v1` only |
| `client/python/` | SDK port | `client/cpp/` public headers |
| `tests/contract/` | architect | contracts, GTest |
| `tests/unit/`, `tests/integration/`, `tests/end-to-end/` | component owners | anything |
| unported legacy files inside `src/chrono-*/`, `client/`, `plugins/` | nobody | not built; read-only reference until the PR that ports them deletes them |

Rules:
- M11.1 Contracts MUST NOT include generated types (W10.7).
- M11.2 No service directory MAY include another service directory. Shared code goes to `src/chrono-common/`.
- M11.3 `src/chrono-common/` MUST NOT include gRPC or protobuf. Transport and serialization live in each service's `adapter/`.
- M11.4 The thread model is fixed: gRPC callback API, bounded `std::jthread` worker pool, `std::mutex` and `std::condition_variable`. Callbacks enqueue; blocking WAL and HDF5 work runs in workers; stop is cooperative with explicit queue draining. Argobots, Thallium, Margo, Mochi and RDMA MUST NOT appear in any CMake target after PR 1. Gate: `cmake` configure fails if any of those packages is found; CI greps `CMakeLists.txt` and `vcpkg.json`.
- M11.5 Build: C++20, gcc 13.3, cmake 3.28, vcpkg manifest with builtin-baseline `4a1c77189c64dae7afd478333a64d1e604d5dc91`, triplet x64-linux, binary cache. No sudo in any script.
- M11.6 One PR touches one owned directory plus its tests, except an RFC-approved contract PR.
- M11.7 Every blocking call (fsync, HDF5, NFS open) MUST run on a worker, never on a gRPC callback thread. Gate: TSAN run plus a debug assertion `CHRONOLOG_ASSERT_WORKER_THREAD()` in the WAL and HDF5 sinks.
- M11.8 Each service has exactly one `main.cpp` that composes contracts from configuration. No global singletons except the process Clock.

## 12. Failure model

Each failure names the behaviour and the gate. "Lost" below means lost from the system; ACCEPTED data is by definition lost on Keeper crash.

**Keeper crash.** ACCEPTED events are lost. DURABLE events are recovered from the WAL on restart and resent to the Grapher. Retained chunks whose receipt was not settled are resent. Gate: `tests/integration/keeper_wal_restart_test.sh` (TBD, PR 5); `JournalContract.DurableAckSurvivesCrash` (I12.1). A restarted Keeper MUST resume HLC assignment above every frontier it has ever reported: PR 5 persists a reserved HLC high watermark in the WAL and never reports a frontier beyond the persisted reservation (gate: `JournalContract.RestartResumesAboveReportedFrontier (to add, PR 5)`). Before PR 5 a Keeper restart is a new Keeper instance and all its ACCEPTED data is gone by definition. A Read spanning the crash reports laggards until writers resume. Writers MUST re-acquire; the new incarnation sorts after the old one (I7.4).

**Grapher crash.** Receipts are scoped to a Grapher instance; a new instance has a new `grapher_instance` and new numbering. A Keeper MUST NOT settle a pending receipt with a report from another instance (I12.2). Legacy guards: `tests/unit/chrono-keeper/chrono_keeper_retention_store_test.cpp::ReceiptFromAnotherGrapherInstanceIsNotSettled`, `::RestartedGrapherSettlesAResentChunkBelowTheKnownWatermark`, `::DelayedReportFromTheSameGrapherDoesNotUndoANewerOne`. W is recovered from the per-Grapher manifest (`ArchiveManifest::deriveWatermarks`, origin/archive-file-manifest). Contract gates: `TierStoreContract.ManifestRestoresContiguousWatermark`, `JournalContract.ReceiptFromAnotherGrapherInstanceIsNotSettled (to add)`.

**Interrupted chunk stream.** A stream that ends without an OK ChunkReceipt MUST be treated as not delivered; the Keeper MUST mark the chunk send-failed and keep it readable and resendable (I12.3). Legacy guard: `chrono_keeper_retention_store_test.cpp::MarkSendFailedKeepsChunkReadableAndResendable`. Contract gate: `ArchiveTransferTest.PartialStreamYieldsNoReceipt (to add)`. A receipt whose byte count differs from what was sent MUST be treated as a failed send.

**Receipt without persistence.** A covering W alone MUST NOT settle a pending receipt (I12.4). Legacy guards: `chrono_keeper_retention_store_test.cpp::ChunkAckedUnderACoveringWatermarkWaitsForItsReceipt`, `tests/unit/chrono-grapher/story_watermark_registry_test.cpp::ReceiptWhoseEventsSpanTwoWindowsSettlesWhenBothAreWritten`, `::ReceiptNotMergedStaysPending`. Contract gate: `JournalContract.CoveringWatermarkAloneDoesNotSettleReceipt (to add)`.

**Keeper shutdown.** The Keeper MUST hand unshipped chunks to extraction while it still runs, resend chunks with no receipt, and wait up to `shutdown_confirm_timeout_secs` (legacy default 150, `conf/default_conf.json.in` on origin/700-watermark-feedback) for the Grapher to confirm (I12.5). Legacy guards: `chrono_keeper_retention_store_test.cpp::ShutdownWaitEndsWhenTheGrapherConfirmsEveryChunk`, `::ShutdownWaitSendsAgainAChunkTheGrapherNeverWrote`, `::ShutdownWaitGivesUpAtTheTimeout`, `::FlushHandsUnshippedChunksOverWhileExtractionRuns`.

**NFS and shared file systems.** Measured by Kun Feng on ares-comp-[03-08], NFSv3, `acregmax=120` (commits 152dd5a8, 56571da5 on origin/archive-file-manifest):

| Assumption | Result | Measurement |
| --- | --- | --- |
| concurrent O_APPEND from several clients is atomic | FAIL | 66.1% record loss, 6 nodes synthetic; 3.2% in a live 2-Grapher run; 0% with 6 processes on 1 node |
| stat() reflects recent writes promptly | FAIL | 120.14 s detection lag against a 1 s poll |
| compaction is detectable | PASS | via mtime, late never lost |
| link() fails rather than clobbers | PASS | EEXIST |
| readdir sees new files | PASS | no persistent UNRECORDED |

Consequences, all normative: no two processes MAY ever append to one file on a shared file system (per-Grapher manifests, section 13). A poller MUST open() a file before deciding whether it changed; stat() on the path is not a change detector. After the fix the publish-to-visible latency measured median 0.19 s, max 1.00 s at a 1000 ms poll (commit 56571da5). Gates: `tests/unit/chrono-common/archive_manifest_test.cpp::EachWriterAppendsToItsOwnLog`, `::AReaderMergesEveryWritersLog`, `::CompactionLeavesOtherWritersLogsAlone`, `::ATornFinalLineIsDiscardedAndEarlierRecordsSurvive`; `tests/integration/manifest_restart_test.sh`. A Player that starts before any manifest exists MUST adopt one that appears later without restart (third bug in commit 56571da5; gate: `tests/unit/chrono-player/chrono_player_archive_index_test.cpp`, the case for a log created after startup). The archive scanner MUST exclude manifest artifacts (`ArchiveManifest::isManifestArtifact`, commit e02ef673).

**Clock loss.** When the Clock source becomes Unavailable the Keeper MUST keep assigning HLC from last-assigned plus logical ticks. Writer physical readings pass through unchanged. Physical-axis reads are incomplete by definition in v1 (I6.10). From PR 6, reads by physical time against events whose status is not Synced MUST widen the range by the event's bound or, when no bound exists, MUST include the event and set `complete=false` (I12.6). Gates: `ReplayContract.PhysicalRangeHonorsUncertainty (to add, PR 6)`, `ClockContract.UnavailableSourceKeepsHlcAdvancing (to add)`.

**Visor crash.** Catalog state is in SQLite with WAL journal mode and synchronous=FULL, so every acknowledged acquire and incarnation bump survives (section 9). Routes are static until PR 8 so Keepers continue serving. Acquire and release are unavailable until restart; appends under a valid epoch continue. From PR 8 the same operations replicate through NuRaft and a Visor crash fails over. Gate: `tests/integration/visor_restart_test.sh` (TBD).

**Player crash.** Stateless. A client's ReplayStream ends with a non-OK status; the client re-issues Read or Tail from its last position (I6.9). Gate: `tests/contract/client/` test `ClientResumesTailAfterStreamError`.

**Writer crash.** The writer re-acquires and receives a strictly higher incarnation (I3.7, I7.4). Appends from the dead incarnation that arrive late MUST be rejected with FAILED_PRECONDITION once a release or a newer incarnation has been seen; before that they are accepted and sort before the new incarnation. Unacked DURABLE requests from the dead incarnation are indistinguishable from never-sent; the SDK MUST NOT report them as acked. Gate: `JournalContract.ReleasedIncarnationCannotAppend`, `JournalContract.OlderIncarnationIsRejected (to add)`.

**Release fencing lag.** Between Catalog's commit of a release and the Keeper applying that revision, the old incarnation can still append at its Keeper. Those events are valid and sort before the next incarnation because the next incarnation is assigned the same Keeper in the same epoch (I7.5). Release reports `fenced=false` when the Keeper did not confirm within the timeout (W10.12). A Visor restart MUST NOT reuse a revision; the counter is a persisted SQLite row. The Keeper heartbeats immediately after applying the fence (W10.6). Gate: `MetadataStoreContract.ReleaseReportsFenceState`, `MetadataStoreContract.RevisionSurvivesRestart (to add)`.

**Partial manifest line.** A torn final line MUST be discarded and earlier records kept (`parseManifestLine` returns false, `ArchiveManifest.h`). Gate: `archive_manifest_test.cpp::ATornFinalLineIsDiscardedAndEarlierRecordsSurvive`.

## 13. Tiering

Four tiers: Keeper RAM (hot, serves Tail and recent Read), Keeper WAL on local SSD (DURABLE), Grapher archive on local HDD or SSD (HDF5 chunk files), and shared NFS or PFS (the archive root visible to Players). The Grapher is the only writer to the archive; Players are readers. Chunk windows are half-open on the HLC axis (`Chunk{start, end}` in `types.h`), replacing the legacy steady_clock windows.

- I13.1 Files MUST be published (written to a temporary name, fsync'd, renamed, then the containing directory fsync'd) before their manifest record is appended or durability is confirmed, so the manifest never names a partial or unlinked file. A crash between the two loses the trailing record, which only under-reports W. Source: `ArchiveManifest.h` ordering rule. Gate: `TierStoreContract.PublishRecordNamesVisibleCompleteFile` (fault-injected rename).
- I13.2 W MUST stop at the first gap. Gate: `archive_manifest_test.cpp::WatermarkStopsAtTheFirstGap`; `story_watermark_registry_test.cpp::GapHoldsWUntilFilled`; `TierStoreContract.WatermarkCannotJumpGap`.
- I13.3 An empty window MUST be recorded (state Empty) and MUST count toward contiguity. Gate: `archive_manifest_test.cpp::AnEmptyWindowKeepsTheContiguousRunIntact`; `TierStoreContract.EmptyWindowMaintainsContinuity`.
- I13.4 Exempt (salvage) and Failed records MUST NOT advance W. Salvage files MUST remain readable. Gates: `archive_manifest_test.cpp::ExemptRecordsDoNotAdvanceTheWatermark`, `::AFailedWriteIsRecordedWithoutAdvancingTheWatermark`; `story_watermark_registry_test.cpp::WriteFailureBlocksReRegisterBump`; `TierStoreContract.ExemptSalvageReadableWithoutAdvancingWatermark, TierStoreContract.FailedWriteNeverAdvancesWatermark`.
- I13.5 A Deleted record supersedes the publication of the same file for reads. A file removed by retention after it was persisted does not lower W. A recovery-time discovery that a window below W is missing or corrupt is recorded as ManifestState LOST, never lowers W, and any Read over that window returns complete=false with reason SOURCE_FAILED. Gate: `archive_manifest_test.cpp::ADeletedFileStopsCountingAsPublished`, `TierStoreContract.DeletedFileSupersedesPublishedRecord` (asserts exclusion from reads and LOST when below W, not a falling W), `ReplayContract.LostWindowBelowWatermarkIsSourceFailed (to add)`.
- I13.6 W MUST never regress within a live story generation. Gates: `story_watermark_registry_test.cpp::IntervalBelowWIsIgnoredNoRegression`, `chrono_keeper_retention_store_test.cpp::WatermarkRegressionIsIgnored`; `TierStoreContract.WatermarkNeverRegresses (to add)`.
- I13.7 One manifest log and one snapshot per Grapher, named by `manifest_writer`, which MUST be stable across restarts (legacy: the recording group id, commit fc10f10b) and MUST NOT be confused with an event writer_id. Readers merge every pair. Compaction MUST only rewrite the writer's own records. Snapshot replacement is by atomic rename. Gate: `archive_manifest_test.cpp::CompactionLeavesOtherWritersLogsAlone`, `::SnapshotPreservesEveryRecordAndTruncatesTheLog`.
- I13.8 The Keeper frees a chunk only when: shipped, chunk.end <= known W, receipt settled, tail released, not in the extraction queue (`src/chrono-keeper/include/KeeperChunkRetentionStore.h`, origin/700-watermark-feedback). Gate: the three `FreeOrder*` tests in `chrono_keeper_retention_store_test.cpp`; `JournalContract.EvictionRequiresWatermarkAndReceipt (to add)`.
- I13.9 Retention is capped by `retention_cap_mb`; at the cap the Keeper MUST evict tail-only chunks first and MUST NOT evict a chunk that is not yet durable in the archive. Gate: `chrono_keeper_retention_store_test.cpp::CapacityEvictionKeepsChunkRetainedUntilDurable`, `tests/unit/chrono-keeper/chrono_keeper_retention_cap_test.cpp`.
- Legacy knobs carried as defaults until re-measured (`conf/default_conf.json.in`, origin/700-watermark-feedback): keeper `story_chunk_duration_secs` 10, `acceptance_window_secs` 15, `tail_capacity` 65536, `retention_cap_mb` 4096, `watermark_resend_timeout_secs` 300, `archive_visibility_delay_secs` 10, `shutdown_confirm_timeout_secs` 150; grapher `story_chunk_duration_secs` 30, `watermark_report_interval_secs` 1; player `story_chunk_duration_secs` 60; manifest poll 1000 ms (commit fc10f10b).

New knobs introduced by this design. Values marked TBD MUST be set by the PR that introduces the knob and recorded here.

| Knob | Owner | Default | Source |
| --- | --- | --- | --- |
| payload max bytes | Keeper | 1 MiB | section 3 |
| group-commit window | Keeper WAL | 1 ms initial, TBD after measurement | section 5 OPEN |
| causal_floor skew limit | Keeper | TBD | I8.6 |
| heartbeat timeout | Visor Cluster | TBD | MembershipContract.RegisterHeartbeatAndRestartFencing |
| chunk checksum algorithm | Keeper to Grapher | CRC32C | W10.4 |
| manifest fsync per append | Grapher | off | `ArchiveManifest` constructor default, origin/archive-file-manifest |

- I13.10 The WAL is truncated at receipt settlement, the same condition as I13.8, never at W alone. The WAL exists to survive a Keeper crash until the Grapher has written. Gate: `JournalContract.WalTruncatesOnlyAfterReceiptSettlement (to add, PR 5)`.

## 14. Security boundary

Core (this repository) provides no TLS, no authentication, no authorization and no data replication: an event lives on one Keeper and one Grapher archive. Catalog metadata replication is distinct and arrives in PR 8 through NuRaft (section 9). Core assumes a trusted network segment.

- S14.1 The gateway, a separate deployable, does authentication, namespace ACLs at chronicle granularity, redaction of `payload` and of `gen_ai.tool.call.arguments` before append, and an audit log of every Catalog mutation. The log is immutable; nothing in core can scrub a secret after append.
- S14.2 Core MUST NOT grow an auth field in `chronolog.v1`. Identity for audit is carried by the gateway in a request header (name TBD) that core forwards into `attributes` unchanged.
- S14.3 Internal services (`chronolog.internal.v1`) MUST bind only to the cluster interface listed in configuration and MUST refuse to start on 0.0.0.0 unless `--insecure-bind-all` is set. Gate: `tests/integration/bind_guard_test.sh` (TBD).
- S14.4 The SDKs talk to the gateway or directly to core; they MUST NOT embed credentials.
- S14.5 Core MUST NOT log payload bytes at any level. Gate: CI grep of `src/` for `envelope.payload` inside logging macros.

## 15. Test gates

**Contract suites** in `tests/contract/`, one GTest value-parameterized suite per contract: `ClockContract`, `MetadataStoreContract`, `MembershipContract`, `JournalContract`, `TierStoreContract`, `ReplayContract`. Every test named in sections 3 through 13 MUST exist in these suites with that exact name. Each implementation instantiates its suite; an uninstantiated suite is allowed only before its component's port PR.

Index of contract tests. "Landed" tests exist in `tests/contract/*_contract_test.cpp` on this branch. "To add" tests are required by an invariant above and MUST be added by the PR that first instantiates the suite for a real implementation; until then the invariant is enforced by review only.

| Suite | Landed | To add |
| --- | --- | --- |
| ClockContract | HlcMonotonicUnderBackwardPhysicalStep, ObservePreservesReadThenWriteCausality, PhysicalReadingAndConservativeBound, StandardHlcLogicalTick, UnsyncedAndUnavailableHaveNoFiniteBound | UnavailableSourceKeepsHlcAdvancing |
| MetadataStoreContract | AcquireReturnsRouteAndEpoch, AssignedIdsDoNotAliasConcatenatedNames, AssignedKeeperStableWithinWriterEpoch, ChronicleAndStoryCrud, ChronicleTombstonePermanence, DestroyRefusesActiveAcquisitions, EpochCompareAndSet, EpochMustStrictlyIncrease, OldReleaseCannotReleaseNewIncarnation, PersistedIncarnationStrictlyIncreases, ReleaseReportsFenceState, StoryTombstonePermanence | RevisionSurvivesRestart |
| MembershipContract | RegisterHeartbeatAndRestartFencing, RouteSnapshotContainsAllRoles, StaleEpochRejection, UnknownStoryCannotValidate | StaticEpochIsOneAndValidated |
| JournalContract | AcceptedIsExplicitRamReceipt, DurableAckSurvivesCrash, EnvelopeAndPhysicalReadingRoundTrip, GaplessSequenceRejection, HalfOpenRangeAndFrontierIncludesRegisteredWriter, IdempotentRetryReturnsOriginalResult, KeeperAssignsHlcAboveCausalFloor, NoSilentDurabilityDowngrade, PayloadAndTraceContextValidation, PerItemFailureDoesNotEraseSuccessfulBatchItems, PerWriterOrderSurvivesBackwardPhysicalStep, PhysicalRangeIsHalfOpen, StaleEpochReturnsCurrentRoute, ReleasedIncarnationCannotAppend, SealedFrontierExceedsEveryAssignmentWhenNothingPending, IdleRegisteredWriterDoesNotBlockCompleteness, DurableInvisibleUntilFsyncAndSealDoesNotPassPendingHlc (PR 5, skipped on RAM), RestartResumesAboveReportedFrontier (PR 5, skipped on RAM) | RejectsIncompleteEventId, AppendResultCarriesAllFourFields, ItemsAfterAGapAreRejected, FsyncFailureFailsTheGroup, OlderIncarnationIsRejected, AbsurdCausalFloorIsRejected, EvictionRequiresWatermarkAndReceipt, DroppedStoryReportFreesRetainedChunks, ReceiptFromAnotherGrapherInstanceIsNotSettled, CoveringWatermarkAloneDoesNotSettleReceipt, UnassignedKeeperRejectsWriter, PendingFsyncRegisteredWithAssignment (PR 5), FsyncCompletesBetweenScanAndCapDoesNotLoseEvent (PR 5), InFlightAssignmentCannotEscapeSealSnapshot (PR 5), FsyncCompletionFlipsVisibilityUnderWriterLock (PR 5), WriterCreatedBeforeFirstAssignmentIsScanned, WalTruncatesOnlyAfterReceiptSettlement (PR 5), ReassignedWriterObservesOldFrontier (PR 8) |
| TierStoreContract | DeletedFileSupersedesPublishedRecord, EmptyWindowMaintainsContinuity, ExemptSalvageReadableWithoutAdvancingWatermark, FailedWriteNeverAdvancesWatermark, HalfOpenRead, IndependentWriterLogsMergeWithoutSharedAppend, ManifestRestoresContiguousWatermark, PublishRecordNamesVisibleCompleteFile, RotationsNeverOverwriteAndIdentityDeduplicates, TornManifestRecordIsIgnored, WatermarkCannotJumpGap | WatermarkNeverRegresses |
| ReplayContract | CancellationIsIdempotent, CompleteWhenEveryKeeperSealReachesEnd, CompletionNamesLaggardsAndEqualityIsSufficient, CompleteReadIsStableForDurableEvents, FailedSourcePreventsCompleteness, HalfOpenRange, IdleRegisteredWriterDoesNotBlockCompleteness, PhysicalAxisAlwaysIncomplete, ReadEndsWithExactlyOneCompletion, TailNeverClaimsCompleteness, TailResumesExclusivelyAfterPosition, TotalOrderAndEventIdentityDeduplication, TruncatedSourcePreventsCompleteness, UnconfirmedHotEventsBelowBoundarySurvive, UnknownWriterCoveredByItsKeeperSeal | ReadOnTombstonedStoryFails, LostWindowBelowWatermarkIsSourceFailed, TrailerEpochMismatchIsSourceFailed (PR 8), PhysicalRangeHonorsUncertainty (PR 6) |
| Adapter suites (`tests/contract/<service>/`, not value-parameterized) | none | JournalAdapterTest.CancelledStreamKeepsAcceptedItems, JournalAdapterTest.MissingEpochIsInvalidArgument, JournalAdapterTest.UnspecifiedClockStatusIsUnavailable, CatalogAdapterTest.ReleaseWaitsForAppliedRevision, ReplayAdapterTest.UnsetRangeIsInvalidArgument, ArchiveTransferTest.PartialStreamYieldsNoReceipt, ArchiveTransferTest.ChecksumMismatchYieldsNoReceipt, ArchiveTransferTest.StreamCompletesAcrossEpochChange (PR 8), ArchiveTransferTest.FetchHotTicksFrontierBeforeScan, ArchiveTransferTest.FrontierTickOrderedBeforeInsert, AcquisitionWatcherTest.RevisionGapsAreSkipped, AcquisitionWatcherTest.RestartRestoresFencesBeforeAdmission, AcquisitionWatcherTest.AppliedRevisionIsMonotonic, AcquisitionWatcherTest.NewOwnerAdmittedOnlyAfterOldOwnerFenced (PR 8), CatalogAdapterTest.DroppedReleaseRetryReturnsCommittedState, client ClientRetriesOnStaleEpochWithSameEventIds, client ClientResumesTailAfterStreamError |

Which implementation instantiates which suite:

| Suite | Instantiated by | Earliest PR |
| --- | --- | --- |
| ClockContract | `src/chrono-common/` ChronyClock and the test FakeClock | PR 2 (walking skeleton) |
| MetadataStoreContract | `src/chrono-visor/` SQLite store and an in-memory store for tests | PR 2 |
| MembershipContract | `src/chrono-visor/` static-route registry | PR 2 |
| JournalContract | `src/chrono-keeper/` RAM journal (PR 3), WAL journal (PR 5) | PR 3 |
| TierStoreContract | `src/chrono-grapher/` HDF5 and manifest store; `src/chrono-player/` read side | per port order A16.10 |
| ReplayContract | `src/chrono-player/` | per port order A16.10 |

PR ladder. Three numbers are fixed: PR 1 removes the Mochi stack and lands contracts, protos and uninstantiated suites; PR 5 lands the fsync'd WAL so DURABLE exists; PR 8 lands dynamic epochs. PR 6 lands the physical-axis acceptance policy that bounds backdating. PR 2 through PR 4 are the walking skeleton (Visor Catalog, Keeper Journal in RAM, composition with one client). PR 7 follows the domain port order in A16.10. Any renumbering is the orchestrator's and MUST be reflected here.

**The three must-not-lose semantics**, each guarded by named legacy tests that MUST be re-homed under the new fixtures before the legacy file is deleted:

1. Delivery receipt is not persistence: `chrono_keeper_retention_store_test.cpp::ChunkAckedUnderACoveringWatermarkWaitsForItsReceipt`, `::ReceiptFromAnotherGrapherInstanceIsNotSettled`; `story_watermark_registry_test.cpp::ReceiptWhoseEventsSpanTwoWindowsSettlesWhenBothAreWritten`. New: `JournalContract.FsyncFailureFailsTheGroup (to add)`, `JournalContract.DurableAckSurvivesCrash`, `tests/integration/keeper_wal_restart_test.sh`.
2. Contiguous publication and recovery: `archive_manifest_test.cpp::WatermarkStopsAtTheFirstGap`, `::AnEmptyWindowKeepsTheContiguousRunIntact`, `::ExemptRecordsDoNotAdvanceTheWatermark`, `::EachWriterAppendsToItsOwnLog`; `tests/integration/manifest_restart_test.sh`.
3. Replay completeness and identity: `chrono_player_hot_range_split_test.cpp::UnacknowledgedEventsBelowTheBoundaryAreKept`, `::AKeeperThatDidNotAnswerWidensTheArchiveReadAndMarksTheReplyIncomplete`; `chrono_player_replay_event_merge_test.cpp::EventsDifferingOnlyInClientIdAreDifferentEvents`; `chronolog_client_chrono_clock_test.cpp::StrictlyIncreasesEvenWhenRawStepsBackward`.

**Sanitizers.** CI runs the unit and contract suites under ASAN+UBSAN and separately under TSAN. A TSAN report is a failure; suppressions require an RFC.

**ctest gating.** `ctest` MUST pass on every PR. A test MAY be disabled only with a `DISABLED_` prefix and a comment citing the issue. CI prints the disabled-test count; a PR that raises it MUST say so in its description and MUST NOT raise it above the count on `supercomputing-sprint` plus the tests it itself ports. Target at release: 0.

**Proto compatibility.** `buf breaking` against the PR 1 baseline on every PR (W10.1). `buf lint` with the DEFAULT rule set and exactly one exception, SERVICE_SUFFIX, so services stay Catalog, Journal, Replay, Archive and Cluster. Every enum value is prefixed with its enum name and the zero value is `*_UNSPECIFIED`.

**Integration and end-to-end.** `manifest_restart_test.sh`, `clock_skew_harness.sh`, and the TBD `keeper_wal_restart_test.sh`, `visor_restart_test.sh`, `bind_guard_test.sh` run nightly and before any release tag. They are not required per PR.

**No latency assertions** in any test until a measurement is recorded in the PR or issue that introduces the claim, naming hardware, build type and commit (section 18).

## 16. Agent operating rules

- A16.1 Every agent works in its own git worktree off `supercomputing-sprint`. Never `cd` into another worktree. Never bare `git stash`.
- A16.2 One PR touches one owned directory (section 11 table) plus its tests. A PR that must touch two needs an RFC.
- A16.3 A contract or proto change needs: an RFC drafted as markdown in the orchestrator's scratch directory outside git, review by two reviewer agents (Codex GPT-6.1 Sol and Fable 5.1 unless the orchestrator names others), and Kun Feng's QA sign-off. The durable record is the diff to this file plus the PR description, which names both reviewers and the QA sign-off.
- A16.4 Agent reports go to the orchestrator's scratch directory outside git. Never a report, plan, summary, evidence, results or audit file in any git tree. Print only the path in the pane.
- A16.5 No human-time estimates anywhere. Progress is gated by tests.
- A16.6 No sudo. No Spack. No network installs outside vcpkg's binary cache.
- A16.7 Ports happen in place inside `src/chrono-*/`, `client/cpp/`, `client/python/`. New subdirectories inside them are fine (`src/chrono-keeper/adapter/`, `src/chrono-keeper/wal/`). No new top-level `src/` directory. Unported legacy files are not built and are reference only; an agent MUST NOT patch them, and the PR that ports a file deletes it.
- A16.8 Every PR description lists which invariants (by number) its tests cover and which legacy guard tests it re-homed.
- A16.9 Agents MUST NOT write the construction "[noun] - [parenthetical clause]" with a dash as a clause separator anywhere.
- A16.10 Walking-skeleton order governs service ports: Visor Catalog, then Keeper Journal, then compose. The domain-class order underneath is: common domain and client Clock/identity, player pure split and merge, common TierStore and manifest with grapher, keeper Journal and WAL, visor, player service, client/cpp, client/python.
- A16.11 An agent that finds a rule in this file wrong reports it in its report file with the reason and proceeds as written. It does not deviate.
- A16.12 Measurements are recorded in the PR or issue that introduces the claim, naming hardware, build type and commit. No evidence or results directories in git. Kun Feng's note applies: LOG_DEBUG is compiled out of Release, so audited runs are Debug and log rotation must be raised before an audited run (commit 56571da5 used 256 MB x 10).

## 17. Migration from legacy

Hard-cut principles:
1. PR 1 removes Mochi, Thallium, Margo, Argobots, RDMA and Spack from the build and lands the six contracts, the two proto files and the uninstantiated contract suites.
2. Port in place inside the kept directories (A16.7). Unported files are removed from the build in PR 1 and deleted by the PR that ports them.
3. Keep the semantics, replace the transport. Every KEEP row's listed tests MUST pass against the port before the legacy file goes.
4. Nothing from the REPLACE rows is copied. Their delivery and concurrency assertions are rewritten as new fixtures.
5. Time axis changes: legacy chunk windows were steady_clock ns mapped by the Visor offset; new windows are HLC. Ported tests MUST substitute Hlc values and MUST keep their window arithmetic.
6. Branch sources: B = origin/700-watermark-feedback, C = origin/visor-clock-exchange, M = manifest commits 625c669b..e02ef673 (re-ported onto B), V = origin/ctx-viz-integration. U = tests/unit/.

| Legacy path | Contract | Keep or replace | Guard tests |
| --- | --- | --- | --- |
| chrono-common StoryChunk, LogEvent, EventSequence, StoryPipeline, ingestion handles and queues | Journal | KEEP | U/chrono-common/chrono_common_story_chunk_test.cpp, chrono_common_story_pipeline_test.cpp |
| chrono-common StoryChunkWriter, LogEventHVL, StoryChunkHVL, HDF5 helpers, ChunkExtractorCSV, drain and flush policy | TierStore | KEEP | chrono_common_story_chunk_writer_test.cpp, chrono_common_chunk_extractor_csv_test.cpp, chrono_common_extraction_module_test.cpp, M story_chunk_writer_test.cpp |
| chrono-common ArchiveManifest, ArchiveManifestRecord (M) | TierStore | KEEP | M U/chrono-common/archive_manifest_test.cpp |
| chrono-common ReceiptTracker, ChunkReceipt, StoryWatermarkReport (B) | TierStore | KEEP, `dropped` flag replaces UINT64_MAX sentinel | B U/chrono-grapher/story_watermark_registry_test.cpp |
| chrono-common ConfigurationBlocks, ConfigurationManager, ExtractionModuleConfiguration | Membership | KEEP, drop transport settings | chrono_common_data_store_conf_test.cpp, chrono_common_extraction_chain_config_test.cpp, chrono_common_conf_template_test.cpp |
| chrono-common RDMATransferAgent, ChunkExtractorRDMA, StoryChunkConsumerService transport, bulk serialization, ULT scheduling | none | REPLACE | new fixtures under tests/contract/ and tests/unit/common/ |
| visor Chronicle, Story, ChronicleMetaDirectory, acquisition policy from VisorClientPortal | MetadataStore | KEEP | tests/integration/client/client_metadata_rpc_test.cpp, tests/end-to-end/destructive-apis/ |
| visor ClientInfo, ClientRegistryInfo, ClientRegistryManager, KeeperRegistry, process entries, RecordingGroup | Membership | KEEP, split policy from endpoints | none in legacy; new MembershipContract instantiation |
| visor ClientPortalService, KeeperRegistryService, DataStoreAdminClient, engine, registration transport | none | REPLACE | MembershipContract, MetadataStoreContract |
| visor clock exchange (C) | Clock | REPLACE by chrony-backed Clock, keep clamp and test seam, keep Cluster.ReadClock in `chronolog.internal.v1` as observability | C U/chronolog-client/chronolog_client_chrono_clock_test.cpp, tests/end-to-end/clock-skew/clock_skew_harness.sh |
| keeper KeeperStoryPipeline, KeeperDataStore, IngestionQueue, StoryIngestionHandle, KeeperChunkRetentionStore, KeeperExtractionChain policy | Journal | KEEP, add WAL | U/chrono-keeper/chrono_keeper_{retention_store,retention_cap,orphan_recovery,release_dataloss,story_retirement,extraction_chain}_test.cpp |
| keeper ActiveTailSource, retained-tail and range read policy | Replay | KEEP | chrono_keeper_retention_store_test.cpp FetchRange* and TailReads* |
| keeper KeeperRecordingService, DataStoreAdminService, KeeperRegClient, RDMA extractors, engine, ULT workers | none | REPLACE | JournalContract; keeper_wal_restart_test.sh (TBD) |
| grapher GrapherDataStore, GrapherExtractionChain, HDF5FileChunkExtractor, StoryWatermarkRegistry, ChunkIngestionQueue policy | TierStore | KEEP | U/chrono-grapher/{hdf5_file_chunk_extractor,story_watermark_registry,grapher_destroy,grapher_pipeline_retirement}_test.cpp, M tests/functional/chrono-grapher/chrono_grapher_archive_manifest_test.cpp |
| grapher ChronoGrapherConfiguration | Membership | KEEP | U/chrono-grapher/grapher_configuration_test.cpp |
| grapher GrapherRecordingService, DataStoreAdminService, GrapherRegClient, WatermarkReportPublisher transport, engine, ULT loop | none | REPLACE, keep report semantics under TierStore | TierStoreContract |
| player HotRangeSplit, ReplayPlan, ReplayEventMerge, PlayerDataStore, ArchiveReadingRequestQueue | Replay | KEEP | U/chrono-player/chrono_player_{hot_range_split,replay_event_merge}_test.cpp, player_pipeline_retirement_test.cpp |
| player HDF5ArchiveReadingAgent index and read logic | TierStore | KEEP | U/chrono-player/chrono_player_{hdf5_archive_reader,archive_client_id,archive_numbered_files,archive_probe,archive_visibility,fs_monitoring}_test.cpp, M chrono_player_archive_index_test.cpp |
| player PlaybackService transport, KeeperHotFetchClient, QueryResponseTransferAgent, registration and admin providers, engine, ABT readers | none | REPLACE | ReplayContract |
| client/cpp ChronoClock, ClockState, ProcessClock, monotonic_clamp (C) | Clock | KEEP clamp and seam, replace exchange | C chronolog_client_chrono_clock_test.cpp |
| client/cpp ClientIdentity | Membership | KEEP | U/chronolog-client/chronolog_client_identity_test.cpp |
| client/cpp Event, playback and tail facade, V callback ownership | Replay | KEEP | V commits dc903e86, 9981a6a3: callback runs on the caller's thread, never on the receive thread |
| client/cpp Client catalog facade | MetadataStore | KEEP | client_metadata_rpc_test.cpp |
| client/cpp StoryHandle write facade | Journal | KEEP | new ClientRetriesOnStaleEpochWithSameEventIds, ClientResumesTailAfterStreamError |
| client/cpp ChronologClientImpl RPC and engine, rpcVisorClient, KeeperRecordingClient, PlaybackQueryRpcClient, ClientQueryService, transport in StorytellerClient and KeeperTailReader | none | REPLACE | SDK contract tests in tests/contract/client/ |
| client/python PyStoryHandle and pybind bindings | Journal, Replay | KEEP, replace endpoint and transport bindings | existing python tests under client/python/ |
| tests/communication/thallium_*, client_{hybrid,multi}_argobots_test.cpp, RDMA and bulk fixtures, ABT-yield in grapher_data_collection_report_test.cpp | none | DELETE, keep assertions in new fixtures | n/a |

Wire-level replacements: Keeper to Grapher chunk transfer becomes Archive.TransferChunk (W10.4). Grapher to Keeper watermark reports become Archive.WatchWatermarks. The legacy client protocol renumbered v4 on C is superseded by `chronolog.v1`; its version field is not carried forward.

Reading an invariant number: I = data and behaviour invariant (sections 3 through 8, 12, 13), W = wire rule (section 10), M = module rule (section 11), S = security rule (section 14), A = agent rule (section 16). A PR description cites these codes.

## 18. Non-goals

- Semantic memory, embeddings, retrieval or any query beyond time range and tail.
- Latency or throughput claims of any kind without a measurement recorded in the PR or issue that makes the claim, naming hardware, build type and commit.
- Cryptographic or tamper-evident receipts. A ChunkReceipt is a counter and a transfer checksum (CRC32C by default), not a proof against a malicious Grapher.
- TLS, authentication, authorization, or replication in core (section 14).
- Dynamic ownership changes before PR 8 (section 4).
- ML-based clock bounds in v1 (section 8).
- Preserving the legacy client protocol, Thallium wire format, or Spack packaging.
- Multi-Keeper write replication for one writer (I7.5). A Route may list several Keepers for partitioning, never for redundancy, in v1.
