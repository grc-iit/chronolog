#pragma once

#include "chronolog/types.h"

namespace chronolog
{

class Membership
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
    virtual ~Membership() = default;

    /**
     * Look up the story ownership route.
     * Preconditions: Story is registered.
     * Postconditions: Returns coherent epoch, Keeper endpoints, Grapher and Player endpoints.
     * Status codes: OK; NOT_FOUND for unknown story; UNAVAILABLE for registry failure.
     * Thread safety: Snapshot copy under a bounded shared-lock section; safe concurrently; copy may allocate.
     * Invariant tests: tests/contract/membership_contract_test.cpp: RouteSnapshotContainsAllRoles,
     * UnknownStoryCannotValidate.
     */
    virtual absl::StatusOr<Route> route(StoryId id) const = 0;
    virtual absl::StatusOr<RouteState> routeState(StoryId id) const
    {
        auto r = route(id);
        if(!r.ok())
            return r.status();
        RouteState state;
        state.route = *r;
        return state;
    }

    /**
     * Check append ownership against the current route epoch.
     * Preconditions: Story is registered; caller presents its acquired epoch.
     * Postconditions: OK iff epoch is current; stale epoch causes no mutation.
     * Status codes: OK; NOT_FOUND for unknown story; FAILED_PRECONDITION for stale epoch; UNAVAILABLE for
     * registry failure.
     * Thread safety: Linearizable with route changes; safe concurrently.
     * Invariant tests: tests/contract/membership_contract_test.cpp: StaleEpochRejection,
     * UnknownStoryCannotValidate.
     */
    virtual absl::Status validateEpoch(StoryId id, Epoch epoch) const = 0;

    /**
     * Register a process role and restart instance.
     * Preconditions: Nonempty stable id, instance and endpoint; valid role.
     * Postconditions: New instance replaces old instance and fences its heartbeats.
     * Status codes: OK; INVALID_ARGUMENT for invalid process; UNAVAILABLE for registry failure.
     * Thread safety: Linearizable with heartbeat; safe concurrently.
     * Invariant tests: tests/contract/membership_contract_test.cpp: RegisterHeartbeatAndRestartFencing.
     */
    virtual absl::Status registerProcess(Process process) = 0;

    /**
     * Renew liveness for one registered restart instance.
     * Preconditions: Process id and instance are nonempty.
     * Postconditions: Only current instance renews liveness; applied_revision is highest contiguous acquisition
     * revision fully applied by that Keeper, not merely received; stale reports never lower it.
     * Status codes: OK; INVALID_ARGUMENT for empty identifiers; NOT_FOUND for unknown id; FAILED_PRECONDITION
     * for obsolete instance; UNAVAILABLE for registry failure.
     * Thread safety: Linearizable with registration; safe concurrently.
     * Invariant tests: tests/contract/membership_contract_test.cpp: RegisterHeartbeatAndRestartFencing.
     */
    virtual absl::Status heartbeat(std::string id, std::string instance, uint64_t applied_revision = 0) = 0;
};

} // namespace chronolog
