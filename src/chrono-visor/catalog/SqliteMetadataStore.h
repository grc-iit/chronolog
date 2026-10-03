#pragma once

#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <semaphore>
#include <vector>
#include <string>

#include <sqlite3.h>

#include "catalog/AcquisitionLedger.h"
#include "chronolog/metadata_store.h"
#include "catalog/LeaseAuthority.h"
#include "membership/Topology.h"
#include "chronolog/internal/v1/internal.pb.h"

namespace chronolog::visor
{
class SqliteMetadataStore;
namespace dynamic
{
internal::v1::MembershipState snapshot(SqliteMetadataStore&);
}

// Durable Catalog state in one SQLite file, WAL journal mode, synchronous=FULL.
// Every mutator is one BEGIN IMMEDIATE transaction and returns after COMMIT, so
// an acknowledged acquire or incarnation bump survives a crash.
class SqliteMetadataStore final
    : public MetadataStore
    , public AcquisitionLedger
{
public:
    // `fence_waiter` may be empty, in which case release always reports fenced=false.
    static absl::StatusOr<std::unique_ptr<SqliteMetadataStore>> open(const std::string& path,
                                                                     Topology topology,
                                                                     FenceWaiter fence_waiter = nullptr,
                                                                     AcquisitionLeaseConfig leases = {},
                                                                     bool replica = false);
    ~SqliteMetadataStore() override;

    SqliteMetadataStore(const SqliteMetadataStore&) = delete;
    SqliteMetadataStore& operator=(const SqliteMetadataStore&) = delete;

    absl::StatusOr<Chronicle> createChronicle(std::string name) override;
    absl::StatusOr<Chronicle> getChronicle(std::string name) const override;
    absl::StatusOr<std::vector<Chronicle>> listChronicles() const override;
    absl::Status destroyChronicle(std::string name) override;
    absl::StatusOr<Story> createStory(std::string chronicle, std::string name) override;
    absl::StatusOr<Story> getStory(StoryId id) const override;
    absl::StatusOr<std::vector<Story>> listStories(std::string chronicle) const override;
    absl::Status destroyStory(StoryId id) override;
    // Waits for the previous owner's fence when the writer moves to another Keeper (I6.11(d)), then commits.
    absl::StatusOr<Acquisition> acquire(StoryId id, std::string writer_identity) override;
    absl::StatusOr<Acquisition> acquire(StoryId id, std::string writer_identity, AcquireOptions options) override;
    absl::StatusOr<std::vector<RenewAcquisitionResult>>
    renewAcquisitions(const std::vector<RenewAcquisition>& acquisitions) override;
    LeaseAuthority& leaseAuthority() { return leases_; }
    absl::Status reconcileLeases();
    absl::Status serviceTick();

    // The commit without the wait, for Raft apply, which must be deterministic and never blocks.
    absl::StatusOr<Acquisition> acquireAfterFence(StoryId id, std::string writer_identity);
    absl::StatusOr<Acquisition> acquireAfterFence(StoryId id,
                                                  std::string writer_identity,
                                                  AcquireOptions options,
                                                  int64_t duration_ns,
                                                  const internal::v1::AcquireCommand* selected = nullptr);
    absl::StatusOr<std::vector<AcquisitionChange>> acquisitionRows(const std::vector<RenewAcquisition>& tuples) const;
    absl::StatusOr<AcquisitionSnapshot> scanAcquisitions(std::pair<StoryId, uint64_t> after, size_t limit) const;
    absl::StatusOr<internal::v1::AcquireCommand> prepareAcquire(const v1::AcquireRequest& request,
                                                                int64_t duration_ns) const;
    void setLeaseObserver(AcquisitionObserver* observer);
    uint64_t publishedAppliedIndex() const { return applied_index_.load(); }

    // OK when the writer's previous incarnation stays on a Keeper of the current route, has no prior release, or its
    // old owner is fenced; UNAVAILABLE while a live old owner has not applied the release.
    absl::Status awaitOldOwnerFence(StoryId id, const std::string& writer_identity) const;
    // True once the Keeper applied `revision` or can no longer serve (not alive). Unset means no wait.
    void setOwnerFence(FenceWaiter fence);
    absl::StatusOr<ReleaseResult> release(StoryId id, uint64_t writer_id, uint64_t incarnation) override;
    absl::StatusOr<Epoch> compareAndSetEpoch(StoryId id, Epoch expected, Epoch desired) override;

    absl::StatusOr<AcquisitionSnapshot> snapshotAcquisitions() const override;
    void setObserver(AcquisitionObserver* observer) override;
    absl::StatusOr<Acquisition> requestGrant(const std::string& request_id) const override;

    absl::StatusOr<std::string> applyRaft(uint64_t index, const std::function<std::string()>& apply);
    absl::StatusOr<uint64_t> appliedIndex() const;
    // Rows written on this connection, including Raft bookkeeping, for apply-cost tests.
    int64_t totalChanges() const;
    using RouteSignal = std::counting_semaphore<>;
    // The signal releases after a route-history commit; consumers read history on their own thread.
    std::shared_ptr<RouteSignal> watchRouteChanges() const;
    absl::StatusOr<KeeperRef> releasedKeeper(StoryId id, uint64_t writer, uint64_t incarnation) const;
    absl::Status backupTo(const std::string& path) const;
    absl::Status installFrom(const std::string& path);
    uint64_t snapshotGeneration() const { return snapshot_generation_.load(); }

    absl::StatusOr<PhysicalPolicy> physicalPolicy() const;
    absl::Status clearPhysicalPolicy(const std::vector<StoryId>& stories);
    absl::Status registerStaticPolicy(const std::string& process, uint64_t version);
    absl::StatusOr<internal::v1::MembershipState> membershipState() const;
    absl::Status saveMembership(const internal::v1::MembershipState& state);
    absl::StatusOr<internal::v1::MembershipState>
    membershipCommandState(const internal::v1::MembershipCommand& command) const;
    absl::Status saveMembershipChanges(const internal::v1::MembershipState& before,
                                       internal::v1::MembershipState& after);
    absl::StatusOr<internal::v1::RouteUpdate> membershipRouteUpdate(StoryId id) const;
    absl::StatusOr<internal::v1::MembershipState> membershipLivenessState() const;
    absl::StatusOr<internal::v1::MembershipState> membershipRouteChanges(uint64_t revision) const;
    absl::StatusOr<uint64_t> membershipRevision() const;
    bool membershipWouldEmpty(const std::string& id) const;
    absl::StatusOr<std::vector<AcquisitionChange>> storyAcquisitions(StoryId id, bool include_released = false) const;
    absl::StatusOr<Route> membershipRoute(StoryId id) const;
    absl::Status
    fenceRemovedWriters(StoryId id, const Route& route, uint64_t revision, const std::string& replacement = "");
    // Value of `PRAGMA <name>` on this connection, for tests and startup logging.
    absl::StatusOr<std::string> pragmaValue(const std::string& name) const;

private:
    friend internal::v1::MembershipState dynamic::snapshot(SqliteMetadataStore&);
    SqliteMetadataStore(sqlite3* db,
                        Topology topology,
                        FenceWaiter fence_waiter,
                        AcquisitionLeaseConfig leases,
                        bool replica);
    absl::Status initialize();

    LeaseAuthority leases_;
    bool replica_{};
    std::mutex reconciliation_mutex_;
    std::pair<StoryId, uint64_t> reconciliation_cursor_{};
    AcquisitionObserver* lease_observer_{};
    std::atomic<uint64_t> applied_index_{};
    void notify(const AcquisitionChange& change);
    std::atomic<uint64_t> snapshot_generation_{};
    absl::Status initializeMembership();
    absl::Status seedMembershipStory(StoryId id);
    uint64_t routeMutationRevision();
    // Inside the destroy transaction: one fresh acquisition revision, then a tombstoned RouteUpdate per story
    // in membership_history (W10.17). Under Raft apply the command's own revision is the fresh one.
    absl::Status tombstoneStories(const std::vector<StoryId>& stories);
    void finishRouteTransaction(bool committed);
    bool route_changes_pending_{};
    mutable std::vector<std::weak_ptr<RouteSignal>> route_signals_;
    sqlite3* db_;
    const Topology topology_;
    const FenceWaiter fence_waiter_;
    FenceWaiter owner_fence_;
    // One mutex serializes every statement on the single connection. const readers
    // lock it too, so it is mutable.
    mutable std::recursive_mutex mutex_;
    AcquisitionObserver* observer_{};
    std::optional<uint64_t> apply_revision_;
    bool applying_{};
    uint64_t applying_index_{};
};

} // namespace chronolog::visor
