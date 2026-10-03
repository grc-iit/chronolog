// Static destroy proof (I3.6, RFC-G section 4) and static expiry over the SQLite authority. Fence proof is
// StaticRouteMembership::waitApplied on the owning process's current instance, as main.cpp wires it; heartbeat
// silence and old-instance reports never count.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

#include "TestSupport.h"
#include "catalog/SqliteMetadataStore.h"
#include "chronolog/acquire_refusal.h"
#include "membership/StaticRouteMembership.h"

namespace chronolog::visor
{
namespace
{
using namespace std::chrono_literals;

class StaticDestroyTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        membership_ = std::make_unique<StaticRouteMembership>(
                testing::twoKeeperTopology(),
                1,
                [](StoryId) { return true; },
                1s,
                [this] { return now_; });
        auto opened = SqliteMetadataStore::open((dir_.path() / "catalog.sqlite").string(),
                                                testing::twoKeeperTopology(),
                                                [this](const KeeperRef& keeper, uint64_t revision) {
                                                    return membership_->waitApplied(keeper.process_id, revision, 50ms);
                                                });
        ASSERT_TRUE(opened.ok()) << opened.status();
        store_ = std::move(*opened);
        ASSERT_TRUE(store_->createChronicle("c").ok());
        story_ = store_->createStory("c", "s")->id;
    }
    void registerOwner(const std::string& process, const std::string& instance)
    {
        ASSERT_TRUE(
                membership_->registerProcess(Process{process, instance, process + ":50052", ProcessRole::Keeper}).ok());
    }
    absl::Status heartbeat(const std::string& process, const std::string& instance, uint64_t applied)
    {
        return membership_->heartbeat(process, instance, applied);
    }
    // The owner's release revision after `holder` expires through the static sweep.
    uint64_t expire(const Acquisition& holder)
    {
        store_->leaseAuthority().advanceClock(holder.lease.duration_ns + 1, true);
        EXPECT_TRUE(store_->sweepExpiry().ok());
        auto results = store_->renewAcquisitions({{holder.story_id, holder.writer_id, holder.incarnation}});
        EXPECT_TRUE(results.ok());
        EXPECT_EQ(results->front().termination_cause, AcquisitionTerminationCause::Expired);
        return store_->release(holder.story_id, holder.writer_id, holder.incarnation)->revision;
    }
    // Supersedes `holder` by own-prior CAS, releases the successor, and returns the supersession revision.
    uint64_t supersede(const Acquisition& holder, const std::string& identity)
    {
        AcquireOptions options;
        options.takeover = true;
        options.expected_prior_incarnation = holder.incarnation;
        options.acquire_request_id = newAcquireRequestId();
        auto successor = store_->acquire(story_, identity, options);
        EXPECT_TRUE(successor.ok()) << successor.status();
        EXPECT_TRUE(store_->release(story_, successor->writer_id, successor->incarnation).ok());
        return store_->release(holder.story_id, holder.writer_id, holder.incarnation)->revision;
    }

    testing::TempDir dir_;
    StaticRouteMembership::TimePoint now_ = std::chrono::steady_clock::now();
    std::unique_ptr<StaticRouteMembership> membership_;
    std::unique_ptr<SqliteMetadataStore> store_;
    StoryId story_{};
};

TEST_F(StaticDestroyTest, SilentOwnerBeyondHeartbeatTimeoutStillRefuses)
{
    for(bool expired: {true, false})
    {
        SCOPED_TRACE(expired ? "EXPIRED" : "SUPERSEDED");
        auto story = store_->createStory("c", expired ? "expired" : "superseded");
        ASSERT_TRUE(story.ok());
        story_ = story->id;
        const std::string identity = expired ? "silent-expired" : "silent-superseded";
        auto holder = store_->acquire(story_, identity);
        ASSERT_TRUE(holder.ok());
        const auto owner = holder->assigned_keeper.process_id;
        registerOwner(owner, "i1");
        ASSERT_TRUE(heartbeat(owner, "i1", 0).ok());
        const auto revision = expired ? expire(*holder) : supersede(*holder, identity);
        // The owner stops heartbeating long past heartbeat_timeout_ms: alive() is false, which the acquire
        // owner fence accepts but static destroy proof never does.
        now_ += 10s;
        ASSERT_FALSE(membership_->alive(owner));
        EXPECT_EQ(store_->destroyStory(story_).code(), absl::StatusCode::kFailedPrecondition);
        EXPECT_FALSE(store_->getStory(story_)->tombstoned);
        ASSERT_TRUE(heartbeat(owner, "i1", revision - 1).ok());
        EXPECT_EQ(store_->destroyStory(story_).code(), absl::StatusCode::kFailedPrecondition);
        ASSERT_TRUE(heartbeat(owner, "i1", revision).ok());
        EXPECT_TRUE(store_->destroyStory(story_).ok());
        EXPECT_TRUE(store_->getStory(story_)->tombstoned);
    }
}

TEST_F(StaticDestroyTest, ReplacementInstanceCannotConfirmAnUnappliedExpiry)
{
    for(bool expired: {true, false})
    {
        SCOPED_TRACE(expired ? "EXPIRED" : "SUPERSEDED");
        auto story = store_->createStory("c", expired ? "replaced-expired" : "replaced-superseded");
        ASSERT_TRUE(story.ok());
        story_ = story->id;
        const std::string identity = expired ? "replaced-expired" : "replaced-superseded";
        auto holder = store_->acquire(story_, identity);
        ASSERT_TRUE(holder.ok());
        const auto owner = holder->assigned_keeper.process_id;
        const std::string old_instance = std::string("old-") + (expired ? "e" : "s");
        const std::string new_instance = std::string("new-") + (expired ? "e" : "s");
        registerOwner(owner, old_instance);
        const auto revision = expired ? expire(*holder) : supersede(*holder, identity);
        // A replacement registers before the old instance reports: registration resets proof to zero, and
        // the obsolete instance's report is rejected rather than standing in for the current one.
        registerOwner(owner, new_instance);
        EXPECT_EQ(heartbeat(owner, old_instance, revision).code(), absl::StatusCode::kFailedPrecondition);
        EXPECT_EQ(store_->destroyStory(story_).code(), absl::StatusCode::kFailedPrecondition);
        ASSERT_TRUE(heartbeat(owner, new_instance, revision - 1).ok());
        EXPECT_EQ(store_->destroyChronicle("c").code(), absl::StatusCode::kFailedPrecondition);
        // The replacement's snapshot application through R proves the fence.
        ASSERT_TRUE(heartbeat(owner, new_instance, revision).ok());
        EXPECT_TRUE(store_->destroyStory(story_).ok());
    }
}

TEST(CatalogLeaseTest, RenewDoesNoReconciliationScan)
{
    testing::TempDir dir;
    auto store = SqliteMetadataStore::open((dir.path() / "catalog.sqlite").string(), testing::twoKeeperTopology());
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->createChronicle("c").ok());
    const auto story = (*store)->createStory("c", "s")->id;
    auto grant = (*store)->acquire(story, "renewing");
    ASSERT_TRUE(grant.ok());
    const RenewAcquisition tuple{story, grant->writer_id, grant->incarnation};
    const auto scans = (*store)->leaseAuthority().reconciliations();
    for(int call = 0; call < 5; ++call)
    {
        auto renewed = (*store)->renewAcquisitions({tuple});
        ASSERT_TRUE(renewed.ok());
        EXPECT_TRUE(renewed->front().status.ok());
    }
    EXPECT_EQ((*store)->leaseAuthority().reconciliations(), scans);
    // A missing current row is still installed by the per-tuple path, and the scan stays on the service tick.
    (*store)->leaseAuthority().eraseForTest(tuple);
    auto installed = (*store)->renewAcquisitions({tuple});
    ASSERT_TRUE(installed.ok());
    EXPECT_TRUE(installed->front().status.ok());
    EXPECT_EQ((*store)->leaseAuthority().size(), 1u);
    EXPECT_EQ((*store)->leaseAuthority().reconciliations(), scans);
    ASSERT_TRUE((*store)->serviceTick().ok());
    EXPECT_GT((*store)->leaseAuthority().reconciliations(), scans);
}

TEST(CatalogLeaseTest, ExpiryReturnsWithoutWaitingForKeeper)
{
    testing::TempDir dir;
    std::atomic<int> waits{};
    auto store = SqliteMetadataStore::open((dir.path() / "catalog.sqlite").string(),
                                           testing::twoKeeperTopology(),
                                           [&waits](const KeeperRef&, uint64_t)
                                           {
                                               ++waits;
                                               return false;
                                           });
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->createChronicle("c").ok());
    const auto story = (*store)->createStory("c", "s")->id;
    auto grant = (*store)->acquire(story, "unfenced");
    ASSERT_TRUE(grant.ok());
    (*store)->leaseAuthority().advanceClock(grant->lease.duration_ns + 1, true);
    ASSERT_TRUE((*store)->serviceTick().ok());
    // The sweep committed EXPIRED without consulting the Keeper fence; Release polls it independently.
    EXPECT_EQ(waits.load(), 0);
    auto results = (*store)->renewAcquisitions({{story, grant->writer_id, grant->incarnation}});
    ASSERT_TRUE(results.ok());
    EXPECT_EQ(results->front().termination_cause, AcquisitionTerminationCause::Expired);
    auto release = (*store)->release(story, grant->writer_id, grant->incarnation);
    ASSERT_TRUE(release.ok());
    EXPECT_FALSE(release->fenced);
    EXPECT_EQ(waits.load(), 1);
}

TEST(CatalogLeaseTest, ExpiryMakesProgressUnderRenewalLoad)
{
    testing::TempDir dir;
    AcquisitionLeaseConfig config;
    config.acquisition_expiry_batch = 8;
    config.acquisition_renew_batch = 64;
    auto store = SqliteMetadataStore::open((dir.path() / "catalog.sqlite").string(),
                                           testing::twoKeeperTopology(),
                                           nullptr,
                                           config);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->createChronicle("c").ok());
    const auto story = (*store)->createStory("c", "s")->id;
    std::vector<Acquisition> dead, live;
    for(int i = 0; i < 20; ++i)
    {
        auto grant = (*store)->acquire(story, "dead-" + std::to_string(i));
        ASSERT_TRUE(grant.ok());
        dead.push_back(*grant);
    }
    for(int i = 0; i < 200; ++i)
    {
        auto grant = (*store)->acquire(story, "live-" + std::to_string(i));
        ASSERT_TRUE(grant.ok());
        live.push_back(*grant);
    }
    const auto T = dead.front().lease.duration_ns;
    auto renewAll = [&]
    {
        for(size_t begin = 0; begin < live.size(); begin += 64)
        {
            std::vector<RenewAcquisition> batch;
            for(size_t i = begin; i < std::min(live.size(), begin + 64); ++i)
                batch.push_back({story, live[i].writer_id, live[i].incarnation});
            auto renewed = (*store)->renewAcquisitions(batch);
            ASSERT_TRUE(renewed.ok());
            for(const auto& result: *renewed) ASSERT_TRUE(result.status.ok()) << result.status;
        }
    };
    renewAll();
    (*store)->leaseAuthority().advanceClock(T / 2, true);
    renewAll();
    (*store)->leaseAuthority().advanceClock(T / 2 + 1, true);
    // Every live holder renews between sweeps; the bounded sweep still reaches every dead holder.
    for(int round = 0; round < 3; ++round)
    {
        renewAll();
        ASSERT_TRUE((*store)->serviceTick().ok());
    }
    auto snapshot = (*store)->snapshotAcquisitions();
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(snapshot->active.size(), live.size());
    for(const auto& holder: dead)
    {
        auto result = (*store)->renewAcquisitions({{story, holder.writer_id, holder.incarnation}});
        ASSERT_TRUE(result.ok());
        EXPECT_EQ(result->front().termination_cause, AcquisitionTerminationCause::Expired);
    }
}

TEST(CatalogLeaseTest, StaleExpiryCannotReleaseReacquiredLease)
{
    testing::TempDir dir;
    auto store = SqliteMetadataStore::open((dir.path() / "catalog.sqlite").string(), testing::twoKeeperTopology());
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->createChronicle("c").ok());
    const auto story = (*store)->createStory("c", "s")->id;
    auto first = (*store)->acquire(story, "reacquired");
    ASSERT_TRUE(first.ok());
    // The authority selects the old tuple, then the holder releases and acquires again before the commit.
    (*store)->leaseAuthority().advanceClock(first->lease.duration_ns + 1, true);
    auto selected = (*store)->leaseAuthority().dueTuples(8);
    ASSERT_TRUE(selected.ok());
    ASSERT_EQ(selected->size(), 1u);
    ASSERT_TRUE((*store)->release(story, first->writer_id, first->incarnation).ok());
    auto second = (*store)->acquire(story, "reacquired");
    ASSERT_TRUE(second.ok());
    const auto revision = (*store)->snapshotAcquisitions()->revision;
    auto outcomes = (*store)->expireAcquisitions(*selected);
    ASSERT_TRUE(outcomes.ok());
    ASSERT_EQ(outcomes->size(), 1u);
    EXPECT_TRUE(absl::IsFailedPrecondition(outcomes->front().status));
    EXPECT_EQ(outcomes->front().termination_cause, AcquisitionTerminationCause::Released);
    EXPECT_EQ((*store)->snapshotAcquisitions()->revision, revision);
    // The committed no-op resolves the selection; the reacquired lease stays live and renewable.
    (*store)->leaseAuthority().resolve(*selected);
    EXPECT_FALSE((*store)->leaseAuthority().pending(selected->front()));
    auto live = (*store)->renewAcquisitions({{story, second->writer_id, second->incarnation}});
    ASSERT_TRUE(live.ok());
    EXPECT_TRUE(live->front().status.ok()) << live->front().status;
}
} // namespace
} // namespace chronolog::visor
