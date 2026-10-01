#pragma once

#include "chronolog/types.h"

namespace chronolog
{

class Journal
{
public:
    /**
     * Destroy this contract instance.
     * Preconditions: No concurrent calls remain on this object.
     * Postconditions: Owned resources are released; a stream cancels and joins owned work.
     * Status codes: No status return; destruction does not throw.
     * Thread safety: Caller must serialize destruction against other access.
     * Invariant tests: Factory harness ownership exercises destruction in every suite;
     * stream cancellation is covered by replay_contract_test.cpp: CancellationIsIdempotent.
     */
    virtual ~Journal() = default;

    /**
     * Accept a batch and assign Keeper HLCs with per-item results.
     * Preconditions: Batch has one valid story_id and epoch; items have writer identity, live acquisition,
     * envelope and causal floor.
     * Sequence starts at 1.
     * Postconditions: Gapless per-writer acceptance; retries return original result/HLC, no duplicate.
     * Default/Unspecified requests Durable: fsync WAL before success, never downgrade. Only Durable is an ack.
     * Keeper uses standard HLC tick over local physical, last HLC and causal floor.
     * ACCEPTED is visible on acceptance; DURABLE only after fsync. Assignment and insertion
     * are atomic under the writer lock. Pending-fsync registration is in that SAME critical section. From M8
     * a reassigned writer's Keeper observes
     * AcquisitionUpdate.last_reported_frontier before accepting that writer; a Keeper seal cannot pass any
     * pending DURABLE HLC.
     * Status codes: Outer: OK; INVALID_ARGUMENT for malformed batch/durability enum; UNAVAILABLE for
     * whole-service failure. Per-item: OK; INVALID_ARGUMENT for invalid identity/envelope/causal skew; FAILED_PRECONDITION for sequence gap (message includes expected next sequence);
     * FAILED_PRECONDITION for stale epoch, released incarnation or wrong assigned Keeper; UNIMPLEMENTED for unbuilt requested durability; UNAVAILABLE for built durability failing; NOT_FOUND
     * for unknown story/writer; RESOURCE_EXHAUSTED for admission capacity; UNAVAILABLE for WAL/provider failure;
     * Any failed item achieves Durability::Unspecified, never a downgraded success level.
     * Validate external causal floors before infallible HLC assignment. Stale epoch NEVER becomes a gRPC error: adapter returns OK, item
     * FAILED_PRECONDITION/current_route, and optional batch current_route if all rejected for epoch. No
     * trailing metadata.
     * Thread safety: Thread-safe; acceptance, authorization, dedup and HLC assignment linearize per writer; no
     * partial item mutation on failure.
     * Invariant tests: tests/contract/journal_contract_test.cpp: IdempotentRetryReturnsOriginalResult,
     * GaplessSequenceRejection, NoSilentDurabilityDowngrade, DurableAckSurvivesCrash,
     * StaleEpochReturnsCurrentRoute, ReleasedIncarnationCannotAppend.
     */
    virtual absl::StatusOr<std::vector<AppendResult>> append(const AppendBatch& batch,
                                                             Durability durability = Durability::Unspecified) = 0;

    /**
     * Read visible events in a half-open HLC or physical range.
     * Preconditions: Known story and start <= end; physical ranges ignore logical component.
     * Postconditions: Returns deterministic replay order without duplicate EventIds; preserves envelope.
     * Status codes: OK; INVALID_ARGUMENT for invalid range/axis; NOT_FOUND for unknown story; UNAVAILABLE for
     * storage failure.
     * Thread safety: Snapshot each writer under a bounded scan-slice lock; safe with concurrent append.
     * Invariant tests: tests/contract/journal_contract_test.cpp:
     * HalfOpenRangeAndFrontierIncludesRegisteredWriter, PhysicalRangeIsHalfOpen,
     * EnvelopeAndPhysicalReadingRoundTrip.
     */
    virtual absl::StatusOr<std::vector<Event>> read(StoryId id, Range range) const = 0;

    /**
     * Read each registered writer/incarnation Keeper-sealed exclusive frontier.
     * Preconditions: Known story.
     * Postconditions: Includes idle registered writers with their Keeper's shared sealed frontier F:
     * every event below F is visible and no future event can be assigned below F. Tick the Keeper
     * HLC and coordinate with all assignments/insertions; never pass an HLC awaiting fsync.
     * Tick F BEFORE the event scan, then take each writer lock so any in-flight assignment
     * completes insertion before scanning that writer. With group commit use
     * F = min(initial tick, per-writer pending minima read under each writer lock DURING that writer scan).
     * Slow fsync pins this Keeper.
     * After restart assignment exceeds every reported F. M5 persists reserved HLC high watermark
     * and never reports F beyond that persisted reservation; RAM restart is a new Keeper instance.
     * Status codes: OK; NOT_FOUND for unknown story; UNAVAILABLE for registry/storage failure.
     * Thread safety: Coherent snapshot; safe concurrently.
     * Invariant tests: tests/contract/journal_contract_test.cpp: HalfOpenRangeAndFrontierIncludesRegisteredWriter,
     * DurableInvisibleUntilFsyncAndSealDoesNotPassPendingHlc, RestartResumesAboveReportedFrontier,
     * SealedFrontierExceedsEveryAssignmentWhenNothingPending,
     * IdleRegisteredWriterDoesNotBlockCompleteness.
     * M5 gate to add: PendingFsyncRegisteredWithAssignment (TSAN).
     * Adapter/Keeper gates to add: FetchHotTicksFrontierBeforeScan, FrontierTickOrderedBeforeInsert (TSAN).
     */
    virtual absl::StatusOr<std::vector<Frontier>> frontier(StoryId id) const = 0;
    /**
     * Read this Keeper's seal even when the story has no registered writers.
     * Preconditions: Story is served by this Keeper in the current Route. Writer state exists before first HLC assignment; writer iteration covers every pre-tick assignment.
     * Postconditions: Returns F after tick-before-scan, per-writer locking and per-writer pending cap collected under each scan lock;
     * Every event below F is visible and no future event can be assigned below F; F may lag newer visible events.
     * Status codes: OK; NOT_FOUND for unknown story; UNAVAILABLE for seal/storage failure.
     * Thread safety: Safe concurrently; assignment and pending registration share the writer critical section.
     * Invariant tests: journal_contract_test.cpp: SealedFrontierExceedsEveryAssignmentWhenNothingPending,
     * DurableInvisibleUntilFsyncAndSealDoesNotPassPendingHlc; M5 PendingFsyncRegisteredWithAssignment to add.
     */
    virtual absl::StatusOr<Hlc> keeperFrontier(StoryId id) const = 0;
    virtual absl::StatusOr<int64_t> physicalFrontier(StoryId id) const = 0;
};

} // namespace chronolog
