#pragma once

#include <memory>
#include <atomic>
#include <mutex>
#include <string>

#include <sqlite3.h>

#include "catalog/AcquisitionLedger.h"
#include "chronolog/metadata_store.h"
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
    static absl::StatusOr<std::unique_ptr<SqliteMetadataStore>>
    open(const std::string& path, Topology topology, FenceWaiter fence_waiter = nullptr);
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
    absl::StatusOr<Acquisition> acquire(StoryId id, std::string writer_identity) override;
    absl::StatusOr<ReleaseResult> release(StoryId id, uint64_t writer_id, uint64_t incarnation) override;
    absl::StatusOr<Epoch> compareAndSetEpoch(StoryId id, Epoch expected, Epoch desired) override;

    absl::StatusOr<AcquisitionSnapshot> snapshotAcquisitions() const override;
    void setObserver(AcquisitionObserver* observer) override;

    absl::StatusOr<std::string> applyRaft(uint64_t index, const std::function<std::string()>& apply);
    absl::StatusOr<uint64_t> appliedIndex() const;
    absl::StatusOr<KeeperRef> releasedKeeper(StoryId id, uint64_t writer, uint64_t incarnation) const;
    absl::Status backupTo(const std::string& path) const;
    absl::Status installFrom(const std::string& path);
    uint64_t snapshotGeneration() const { return snapshot_generation_.load(); }

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
    absl::StatusOr<std::vector<AcquisitionChange>> storyAcquisitions(StoryId id) const;
    absl::StatusOr<Route> membershipRoute(StoryId id) const;
    absl::Status
    fenceRemovedWriters(StoryId id, const Route& route, uint64_t revision, const std::string& replacement = "");
    // Value of `PRAGMA <name>` on this connection, for tests and startup logging.
    absl::StatusOr<std::string> pragmaValue(const std::string& name) const;

private:
    friend internal::v1::MembershipState dynamic::snapshot(SqliteMetadataStore&);
    SqliteMetadataStore(sqlite3* db, Topology topology, FenceWaiter fence_waiter);
    absl::Status initialize();

    std::atomic<uint64_t> snapshot_generation_{};
    absl::Status initializeMembership();
    absl::Status seedMembershipStory(StoryId id);
    sqlite3* db_;
    const Topology topology_;
    const FenceWaiter fence_waiter_;
    // One mutex serializes every statement on the single connection. const readers
    // lock it too, so it is mutable.
    mutable std::recursive_mutex mutex_;
    AcquisitionObserver* observer_{};
};

} // namespace chronolog::visor
