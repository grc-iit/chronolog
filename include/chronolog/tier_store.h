#pragma once

#include "chronolog/types.h"

namespace chronolog
{

class TierStore
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
    virtual ~TierStore() = default;

    /**
     * Atomically publish a chunk and then append its manifest record.
     * Preconditions: Valid chunk identity, known story, half-open window and bounded event data.
     * Postconditions: Never overwrite existing rotation. File rename and containing-directory fsync precede manifest record; API readers see data only through a manifest record. Empty maintains continuity;
     * failed/exempt never advance W. Stable per-writer logs avoid shared NFS append; no partial file is
     * visible.
     * Status codes: OK; INVALID_ARGUMENT for invalid chunk; NOT_FOUND for unknown story; RESOURCE_EXHAUSTED for
     * full storage; UNAVAILABLE for I/O/publish/manifest failure. A Failed manifest record may be returned with
     * OK but never indicates persisted events.
     * Thread safety: Thread-safe; publication atomic; manifest appends serialized per manifest writer.
     * Invariant tests: tests/contract/tier_store_contract_test.cpp: PublishRecordNamesVisibleCompleteFile,
     * RotationsNeverOverwriteAndIdentityDeduplicates, FailedWriteNeverAdvancesWatermark,
     * IndependentWriterLogsMergeWithoutSharedAppend.
     */
    virtual absl::StatusOr<ManifestRecord> publish(Chunk chunk) = 0;

    /**
     * Read archive and salvage events in a half-open range.
     * Preconditions: Known story and valid range.
     * Postconditions: Includes exempt files and rotations; deduplicates EventId; replay order deterministic.
     * Status codes: OK; INVALID_ARGUMENT for invalid range/axis; NOT_FOUND for unknown story; UNAVAILABLE for
     * archive I/O failure.
     * Thread safety: Coherent published-file snapshot; safe with concurrent publication.
     * Invariant tests: tests/contract/tier_store_contract_test.cpp:
     * ExemptSalvageReadableWithoutAdvancingWatermark, HalfOpenRead,
     * RotationsNeverOverwriteAndIdentityDeduplicates.
     */
    virtual absl::StatusOr<std::vector<Event>> read(StoryId id, Range range) const = 0;

    /**
     * Read a per-story manifest snapshot by value.
     * Preconditions: Known story.
     * Postconditions: Includes all writer logs; ignores torn records; deletion supersedes that file publication; missing/corrupt persisted windows are recorded Lost.
     * Status codes: OK; NOT_FOUND for unknown story; UNAVAILABLE for manifest I/O failure.
     * Thread safety: Coherent snapshot; safe concurrently; no borrowed containers.
     * Invariant tests: tests/contract/tier_store_contract_test.cpp: DeletedFileSupersedesPublishedRecord,
     * TornManifestRecordIsIgnored, IndependentWriterLogsMergeWithoutSharedAppend.
     */
    virtual absl::StatusOr<std::vector<ManifestRecord>> manifest(StoryId id) const = 0;

    /**
     * Compute the longest contiguous persisted prefix from manifest state.
     * Preconditions: Known story with configured start anchor.
     * Postconditions: Cannot jump gaps; includes empty windows, excludes failed/exempt files; W is monotonic within a live generation; retention deletion does not lower W; Lost records preserve W but force incomplete SOURCE_FAILED reads;
     * reconstructs after restart.
     * Status codes: OK; NOT_FOUND for unknown story; UNAVAILABLE for manifest I/O failure.
     * Thread safety: Coherent snapshot; safe with concurrent publish/deletion.
     * Invariant tests: tests/contract/tier_store_contract_test.cpp: WatermarkCannotJumpGap,
     * EmptyWindowMaintainsContinuity, ManifestRestoresContiguousWatermark.
     */
    virtual absl::StatusOr<Hlc> contiguousWatermark(StoryId id) const = 0;
};

} // namespace chronolog
