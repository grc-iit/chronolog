#pragma once

#include "chronolog/types.h"

namespace chronolog
{

class MetadataStore
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
    virtual ~MetadataStore() = default;

    /**
     * Create a named chronicle.
     * Preconditions: Name is nonempty and valid.
     * Postconditions: New live chronicle is durable; Old identities stay tombstoned; a reused name creates a new identity.
     * Status codes: OK; INVALID_ARGUMENT for invalid name; ALREADY_EXISTS for a live name; FAILED_PRECONDITION
     * for tombstoned name; UNAVAILABLE for storage failure.
     * Thread safety: Linearizable; safe concurrently.
     * Invariant tests: tests/contract/metadata_store_contract_test.cpp: ChronicleAndStoryCrud,
     * ChronicleTombstonePermanence.
     */
    virtual absl::StatusOr<Chronicle> createChronicle(std::string name) = 0;

    /**
     * Retrieve live or tombstoned chronicle metadata.
     * Preconditions: Name is valid.
     * Postconditions: Returns the current durable metadata including tombstone.
     * Status codes: OK; INVALID_ARGUMENT for invalid name; NOT_FOUND for unknown name; UNAVAILABLE for storage
     * failure.
     * Thread safety: Linearizable; safe concurrently.
     * Invariant tests: tests/contract/metadata_store_contract_test.cpp: ChronicleAndStoryCrud,
     * ChronicleTombstonePermanence.
     */
    virtual absl::StatusOr<Chronicle> getChronicle(std::string name) const = 0;

    /**
     * List chronicles including tombstones.
     * Preconditions: None.
     * Postconditions: Returns a coherent snapshot by value.
     * Status codes: OK; UNAVAILABLE for storage failure.
     * Thread safety: Snapshot is linearizable; safe concurrently.
     * Invariant tests: tests/contract/metadata_store_contract_test.cpp: ChronicleAndStoryCrud.
     */
    virtual absl::StatusOr<std::vector<Chronicle>> listChronicles() const = 0;

    /**
     * Permanently tombstone a chronicle and every contained story.
     * Preconditions: Chronicle exists; no contained story has an active acquisition.
     * Postconditions: Old identities remain destroyed; operation is atomic across contained stories.
     * Status codes: OK; INVALID_ARGUMENT for invalid name; NOT_FOUND for unknown name; FAILED_PRECONDITION for
     * active acquisitions; UNAVAILABLE for storage failure.
     * Thread safety: Linearizable with acquisition and create operations.
     * Invariant tests: tests/contract/metadata_store_contract_test.cpp: ChronicleTombstonePermanence,
     * DestroyRefusesActiveAcquisitions.
     */
    virtual absl::Status destroyChronicle(std::string name) = 0;

    /**
     * Assign a uint64 story id to a chronicle/name pair.
     * Preconditions: Parent chronicle exists and is live; story name valid.
     * Postconditions: Distinct name pairs have distinct ids; Old ids cannot resurrect; reused names receive fresh ids.
     * Status codes: OK; INVALID_ARGUMENT for invalid names; NOT_FOUND for unknown chronicle; ALREADY_EXISTS for
     * live story; FAILED_PRECONDITION for tombstones; UNAVAILABLE for storage failure.
     * Thread safety: Linearizable; safe concurrently.
     * Invariant tests: tests/contract/metadata_store_contract_test.cpp: AssignedIdsDoNotAliasConcatenatedNames,
     * StoryTombstonePermanence.
     */
    virtual absl::StatusOr<Story> createStory(std::string chronicle, std::string name) = 0;

    /**
     * Retrieve live or tombstoned story metadata by id.
     * Preconditions: Id was assigned by Catalog.
     * Postconditions: Returns a coherent durable metadata snapshot.
     * Status codes: OK; NOT_FOUND for unknown id; UNAVAILABLE for storage failure.
     * Thread safety: Linearizable; safe concurrently.
     * Invariant tests: tests/contract/metadata_store_contract_test.cpp: ChronicleAndStoryCrud,
     * StoryTombstonePermanence.
     */
    virtual absl::StatusOr<Story> getStory(StoryId id) const = 0;

    /**
     * List all stories in a chronicle, including tombstones.
     * Preconditions: Chronicle exists.
     * Postconditions: Returns a coherent snapshot by value.
     * Status codes: OK; INVALID_ARGUMENT for invalid name; NOT_FOUND for unknown chronicle; UNAVAILABLE for
     * storage failure.
     * Thread safety: Linearizable; safe concurrently.
     * Invariant tests: tests/contract/metadata_store_contract_test.cpp: ChronicleAndStoryCrud.
     */
    virtual absl::StatusOr<std::vector<Story>> listStories(std::string chronicle) const = 0;

    /**
     * Permanently tombstone a story.
     * Preconditions: Story exists and has no active acquisitions.
     * Postconditions: Story stays destroyed across restart; acquire cannot resurrect the old id; reused name gets a new id.
     * Status codes: OK; NOT_FOUND for unknown id; FAILED_PRECONDITION for active acquisitions; UNAVAILABLE for
     * storage failure.
     * Thread safety: Linearizable with acquire/release.
     * Invariant tests: tests/contract/metadata_store_contract_test.cpp: StoryTombstonePermanence,
     * DestroyRefusesActiveAcquisitions.
     */
    virtual absl::Status destroyStory(StoryId id) = 0;

    /**
     * Acquire a writer identity for a story.
     * Preconditions: Live story and nonempty stable writer identity. An identity that still holds an active
     * acquisition has crashed: acquire supersedes it by releasing the old incarnation, with its own revision and
     * without a fence wait, in the same atomic step that creates the next incarnation.
     * Postconditions: Returns stable writer_id, persisted strictly increased incarnation, route, epoch and
     * assigned_keeper stable per (writer_id, epoch).
     * Status codes: OK; INVALID_ARGUMENT for empty identity; NOT_FOUND for unknown id; FAILED_PRECONDITION for
     * tombstone; UNAVAILABLE for storage failure.
     * Thread safety: Linearizable; persistence completes before success.
     * Invariant tests: tests/contract/metadata_store_contract_test.cpp: PersistedIncarnationStrictlyIncreases,
     * AcquireReturnsRouteAndEpoch,
     * AssignedKeeperStableWithinWriterEpoch.
     */
    virtual absl::StatusOr<Acquisition> acquire(StoryId id, std::string writer_identity) = 0;

    /**
     * Commit release and report whether the assigned Keeper applied its incarnation fence.
     * Preconditions: Valid story/writer/incarnation tuple; retries of a committed release are allowed.
     * Postconditions: Commit a monotonically revisioned release; wait a configured timeout for Keeper
     * applied_revision. Return fenced=false if unconfirmed; appends accepted before fencing stay valid. Return
     * fenced=true only after confirmation. Newer incarnations remain untouched. Retry returns the same persisted global Catalog revision and current fence state, never allocates a revision.
     * Status codes: OK; NOT_FOUND for unknown tuple; FAILED_PRECONDITION for a stale tuple that would affect a newer acquisition;
     * UNAVAILABLE for storage failure.
     * Thread safety: Catalog commit is linearizable with acquire; remote fencing is asynchronous and explicitly
     * reported.
     * Invariant tests: tests/contract/metadata_store_contract_test.cpp: ReleaseReportsFenceState,
     * OldReleaseCannotReleaseNewIncarnation
     * (append fencing additionally tested in journal_contract_test.cpp: ReleasedIncarnationCannotAppend).
     */
    virtual absl::StatusOr<ReleaseResult> release(StoryId id, uint64_t writer_id, uint64_t incarnation) = 0;

    /**
     * Atomically replace the per-story ownership epoch.
     * Preconditions: Live story; desired strictly exceeds expected. Static epochs until PR 8.
     * Postconditions: Returns desired only when current equals expected; failure does not mutate.
     * Status codes: OK; NOT_FOUND for unknown story; INVALID_ARGUMENT for non-increasing desired;
     * FAILED_PRECONDITION for mismatch/tombstone; UNAVAILABLE for storage failure.
     * Thread safety: Linearizable; safe concurrently.
     * Invariant tests: tests/contract/metadata_store_contract_test.cpp: EpochCompareAndSet,
     * EpochMustStrictlyIncrease.
     */
    virtual absl::StatusOr<Epoch> compareAndSetEpoch(StoryId id, Epoch expected, Epoch desired) = 0;
};

} // namespace chronolog
