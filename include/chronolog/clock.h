#pragma once

#include "chronolog/types.h"

namespace chronolog
{

class Clock
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
    virtual ~Clock() = default;

    /**
     * Read CLOCK_REALTIME and its conservative uncertainty bound.
     * Preconditions: The configured chrony/kernel source is accessible.
     * Postconditions: Synced has a finite ns bound; Unsynced/Unavailable has no bound.
     * Status codes: OK; UNAVAILABLE if the clock provider itself cannot be queried.
     * Thread safety: Concurrent calls return individually coherent snapshots.
     * Invariant tests: tests/contract/clock_contract_test.cpp: PhysicalReadingAndConservativeBound,
     * UnsyncedAndUnavailableHaveNoFiniteBound.
     */
    virtual absl::StatusOr<TimeReading> now() const = 0;

    /**
     * Assign the next local HLC using the standard HLC tick.
     * Preconditions: Local physical time is representable; HLC state initialized.
     * Postconditions: Result strictly exceeds the prior HLC even after a backward physical step. Logical
     * overflow carries into physical_ns.
     * Status codes: None; infallible. Unavailable physical source advances last HLC by logical ticks.
     * Thread safety: Linearizable with tick and observe.
     * Invariant tests: tests/contract/clock_contract_test.cpp: HlcMonotonicUnderBackwardPhysicalStep,
     * StandardHlcLogicalTick, UnavailableSourceKeepsHlcAdvancing.
     */
    virtual Hlc tick() = 0;

    /**
     * Merge a remote HLC and assign a subsequent local HLC.
     * Preconditions: Remote HLC is valid and representable.
     * Postconditions: Result strictly exceeds both the remote HLC and last local HLC.
     * Status codes: None; infallible for valid representable input. Admission checks reject invalid causal floors before observe.
     * Thread safety: Linearizable with tick and observe.
     * Invariant tests: tests/contract/clock_contract_test.cpp: ObservePreservesReadThenWriteCausality.
     */
    virtual Hlc observe(Hlc remote) = 0;

    /**
     * Read the physical-time error bound in nanoseconds.
     * Preconditions: The configured chrony/kernel source is accessible.
     * Postconditions: Bound >= root delay/2 + root dispersion + drift*age; absent if unsynced or unavailable.
     * No learned predictor shrinks it.
     * Status codes: OK; UNAVAILABLE if the bound provider itself cannot be queried.
     * Thread safety: Concurrent calls return individually coherent snapshots.
     * Invariant tests: tests/contract/clock_contract_test.cpp: PhysicalReadingAndConservativeBound,
     * UnsyncedAndUnavailableHaveNoFiniteBound.
     */
    virtual absl::StatusOr<std::optional<uint64_t>> uncertainty() const = 0;
};

} // namespace chronolog
