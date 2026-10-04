#pragma once

#include "chronolog/predicate.h"
#include "chronolog/types.h"

namespace chronolog
{

class ReplayStream
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
    virtual ~ReplayStream() = default;

    /**
     * Pull the next event batch or final Completion.
     * Preconditions: Exactly one consumer per stream.
     * Postconditions: Read ends with one Completion then nullopt EOF. Tail orderly end has complete=false.
     * Cancellation/failure can terminate without Completion.
     * Status codes: OK; CANCELLED after cancellation; UNAVAILABLE for fatal stream failure; RESOURCE_EXHAUSTED
     * for stream buffering limit.
     * Thread safety: Single-consumer; safe concurrently with cancel, never invoke callbacks under locks.
     * Invariant tests: tests/contract/replay_contract_test.cpp: ReadEndsWithExactlyOneCompletion,
     * TailNeverClaimsCompleteness, CancellationIsIdempotent.
     */
    virtual absl::StatusOr<std::optional<ReplayBatch>> next() = 0;

    /**
     * Cancel a stream and unblock its consumer.
     * Preconditions: Stream is alive.
     * Postconditions: Idempotent; next terminates without claiming completeness.
     * Status codes: No status return; noexcept behavior is required.
     * Thread safety: Safe concurrently with next and other cancel calls.
     * Invariant tests: tests/contract/replay_contract_test.cpp: CancellationIsIdempotent.
     */
    virtual void cancel() = 0;
};

class Replay
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
    virtual ~Replay() = default;

    /**
     * Open a replay stream over a half-open range.
     * Preconditions: Known story and valid range.
     * Postconditions: Total order (HLC,writer,incarnation,sequence), identity dedup, preserve unconfirmed hot
     * events. HLC completion iff every Keeper in the story Route answered and its sealed frontier >= end;
     * laggards named. Physical read
     * always incomplete with PhysicalAxisUnbounded. Idle registered writers share their Keeper seal
     * and never block completeness. FetchHot ticks before scanning and takes each writer lock
     * after the tick to await in-flight insertion; its seal is capped by per-writer pending minima read under each writer lock during that writer scan. Pending-fsync registration shares the assignment critical section.
     * Player obtains the complete Keeper set from the story Route. WatchAcquisitions only names laggards; a
     * stale writer view never authorizes completeness.
     * Unanswered route Keeper makes Read incomplete with SOURCE_FAILED, even with no known writer assigned to
     * it. From M8 reassignment
     * observes the old Keeper last_reported_frontier before accepting the writer. Laggards are writers with a
     * missing Keeper response or F < end.
     * Complete reads are stable for DURABLE events; ACCEPTED events can vanish on Keeper crash. Source
     * failure/truncation set corresponding reason.
     * Status codes: OK; INVALID_ARGUMENT for invalid range/axis; NOT_FOUND for unknown story; UNAVAILABLE for
     * whole-query initialization failure; RESOURCE_EXHAUSTED for admission failure. Individual source failures
     * produce incomplete Completion.
     * Thread safety: Independent streams safe concurrently; stream itself single-consumer.
     * Invariant tests: tests/contract/replay_contract_test.cpp: TotalOrderAndEventIdentityDeduplication,
     * CompletionNamesLaggardsAndEqualityIsSufficient, PhysicalAxisAlwaysIncomplete,
     * FailedSourcePreventsCompleteness, TruncatedSourcePreventsCompleteness,
     * IdleRegisteredWriterDoesNotBlockCompleteness, UnknownWriterCoveredByItsKeeperSeal,
     * CompleteReadIsStableForDurableEvents.
     */
    virtual absl::StatusOr<std::unique_ptr<ReplayStream>> read(StoryId id, Range range) const = 0;

    /**
     * Open a replay stream over a half-open range, keeping only the events a predicate matches (I6.17).
     * Preconditions: As read without a predicate.
     * Postconditions: A predicate only removes events: the range, order, sources consulted and Completion rules are
     * those of read without one, applied to the matching events. An empty predicate matches every event.
     * Status codes: As read without a predicate; INVALID_ARGUMENT for a malformed or oversized predicate.
     * Thread safety: As read without a predicate.
     * Invariant tests: tests/contract/replay_contract_test.cpp: PredicatesOnlyRemoveEvents.
     */
    virtual absl::StatusOr<std::unique_ptr<ReplayStream>>
    read(StoryId id, Range range, const EventPredicate& predicate) const = 0;

    /**
     * Open a live subscription exclusively after the supplied position.
     * Preconditions: Known story; position EventId refers to that story.
     * Postconditions: Resumes in total order; never claims completeness; orderly end includes false Completion.
     * Status codes: OK; INVALID_ARGUMENT for mismatched/invalid position; NOT_FOUND for unknown story;
     * UNAVAILABLE for subscription failure; RESOURCE_EXHAUSTED for admission failure.
     * Thread safety: Independent streams safe concurrently; destruction cancels and joins owned work.
     * Invariant tests: tests/contract/replay_contract_test.cpp: TailNeverClaimsCompleteness,
     * TailResumesExclusivelyAfterPosition.
     */
    virtual absl::StatusOr<std::unique_ptr<ReplayStream>> tail(StoryId id, Event position) const = 0;

    /**
     * Open a live subscription exclusively after the supplied position, delivering only matching events (I6.17).
     * Preconditions: As tail without a predicate.
     * Postconditions: A predicate only removes events: the frontier and every delivery rule are those of tail without
     * one, applied to the matching events.
     * Status codes: As tail without a predicate; INVALID_ARGUMENT for a malformed or oversized predicate.
     * Thread safety: As tail without a predicate.
     * Invariant tests: tests/contract/replay_contract_test.cpp: PredicatesOnlyRemoveEvents.
     */
    virtual absl::StatusOr<std::unique_ptr<ReplayStream>>
    tail(StoryId id, Event position, const EventPredicate& predicate) const = 0;
};

} // namespace chronolog
