#include <gtest/gtest.h>
#include <future>
#include <thread>
#include <sys/wait.h>
#include <signal.h>
#include "TestSupport.h"
#include "adapter/Convert.h"
#include "catalog/SqliteMetadataStore.h"
#include "VisorConfig.h"
#include "chronolog/acquire_refusal.h"

namespace chronolog::visor
{
using namespace std::chrono_literals;

TEST(CatalogLeaseTest, QualificationPublicationAndRebuildPreserveOrderedTransitions)
{
    AcquisitionLeaseConfig config;
    LeaseAuthority authority(config, true);
    authority.publish({1, authority.now(), true});
    authority.beginRebuild(1);
    AcquisitionChange first{1, 1, 1, 1, {"keeper", "keeper:1"}, AcquisitionState::Acquired, 300000000000};
    auto terminal = first;
    terminal.revision = 2;
    terminal.state = AcquisitionState::Released;
    terminal.termination_cause = AcquisitionTerminationCause::Released;
    auto second = first;
    second.revision = 3;
    second.incarnation = 2;
    authority.onAcquisitionChange(terminal);
    authority.onAcquisitionChange(second);
    AcquisitionSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.applied_index = 9;
    snapshot.active = {first};
    authority.rebuild(snapshot);
    EXPECT_EQ(authority.size(), 1u);
    EXPECT_TRUE(authority.sample(second, false).ok());
    EXPECT_FALSE(authority.sample(first, true).ok());
    authority.advanceClock(1000000000, true);
    auto before = authority.sample(second, false);
    ASSERT_TRUE(before.ok());
    const auto lost = authority.now();
    authority.publish({1, lost, false});
    EXPECT_TRUE(absl::IsUnavailable(authority.service()));
    authority.advanceClock(600000000000, true);
    authority.publish({1, lost - 1, true});
    EXPECT_TRUE(absl::IsUnavailable(authority.service()));
    authority.publish({1, authority.now(), true});
    ASSERT_TRUE(authority.service().ok());
    auto after = authority.sample(second, false);
    ASSERT_TRUE(after.ok());
    EXPECT_LT(after->remaining_ns, before->remaining_ns);
    EXPECT_GT(after->remaining_ns, before->remaining_ns - 1000000000);
}

TEST(CatalogLeaseTest, RenewalObserverAndReconciliationSerializeWithoutStorageLock)
{
    LeaseAuthority authority;
    AcquisitionChange first{1, 1, 1, 1, {"keeper", "keeper:1"}, AcquisitionState::Acquired, 300000000000};
    authority.onAcquisitionChange(first);
    AcquisitionSnapshot stale;
    stale.revision = 1;
    stale.active = {first};
    auto terminal = first;
    terminal.revision = 2;
    terminal.state = AcquisitionState::Released;
    auto worker = std::async(std::launch::async,
                             [&]
                             {
                                 for(int i = 0; i < 100; ++i) (void)authority.sample(first, true);
                             });
    authority.onAcquisitionChange(terminal);
    authority.reconcile(stale, {}, true);
    worker.get();
    EXPECT_EQ(authority.size(), 0u);
    EXPECT_FALSE(authority.sample(first, true).ok());
    auto second = first;
    second.revision = 3;
    second.incarnation = 2;
    authority.onAcquisitionChange(second);
    AcquisitionSnapshot empty;
    empty.revision = 4;
    authority.reconcile(empty, {}, true);
    auto removed = second;
    removed.revision = 4;
    removed.state = AcquisitionState::Released;
    authority.reconcileTerminals({removed});
    EXPECT_EQ(authority.size(), 0u);
}

TEST(CatalogLeaseTest, DevSchemaUpgradeInitializesFiniteLeases)
{
    testing::TempDir dir;
    const auto path = (dir.path() / "catalog").string();
    sqlite3* database{};
    ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    ASSERT_EQ(
            sqlite3_exec(database,
                         "CREATE TABLE schema_version(version INTEGER NOT NULL); INSERT INTO schema_version VALUES(2);"
                         "CREATE TABLE acquisitions(story_id INTEGER NOT NULL,writer_id INTEGER NOT NULL,incarnation "
                         "INTEGER NOT NULL,released INTEGER NOT NULL,keeper_id TEXT NOT NULL,keeper_endpoint TEXT NOT "
                         "NULL,PRIMARY KEY(story_id,writer_id));"
                         "INSERT INTO acquisitions VALUES(1,1,7,0,'keeper','keeper:1');",
                         nullptr,
                         nullptr,
                         nullptr),
            SQLITE_OK);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    auto store = SqliteMetadataStore::open(path, testing::twoKeeperTopology());
    ASSERT_TRUE(store.ok()) << store.status();
    auto rows = (*store)->snapshotAcquisitions();
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows->active.size(), 1u);
    EXPECT_EQ(rows->active.front().incarnation, 7u);
    EXPECT_EQ(rows->active.front().duration_ns, 300000000000);
    auto renewed = (*store)->renewAcquisitions({{1, 1, 7}});
    ASSERT_TRUE(renewed.ok());
    EXPECT_TRUE(renewed->front().status.ok());
}

TEST(CatalogLeaseTest, InvalidLeaseConfigurationFailsAtStartup)
{
    VisorConfig config;
    EXPECT_TRUE(config.validate().ok());
    const auto floor = int64_t(std::max(config.raft.election_upper_ms, config.heartbeat_timeout_ms) +
                               config.release_fence_timeout_ms + config.leases.lease_safety_margin_ms) *
                       1000000;
    config.leases.acquisition_lease_min_ns = floor;
    EXPECT_TRUE(absl::IsInvalidArgument(config.validate()));
    config.leases.acquisition_lease_min_ns = floor + 1;
    EXPECT_TRUE(config.validate().ok());
    config.leases.acquisition_service_gap_ms = config.leases.lease_safety_margin_ms + 1;
    EXPECT_TRUE(absl::IsInvalidArgument(config.validate()));
}

TEST(CatalogLeaseTest, RealServiceStallPausesDeadlines)
{
    // The child uses real CLOCK_MONOTONIC. Its parent freezes the whole authority process.
    int ready[2], resume[2];
    ASSERT_EQ(pipe(ready), 0);
    ASSERT_EQ(pipe(resume), 0);
    const auto child = fork();
    ASSERT_GE(child, 0);
    if(!child)
    {
        close(ready[0]);
        close(resume[1]);
        AcquisitionLeaseConfig config;
        config.acquisition_service_tick_ms = 2;
        config.acquisition_service_gap_ms = 20;
        LeaseAuthority authority(config);
        AcquisitionChange live{1, 1, 1, 1, {}, AcquisitionState::Acquired, 100000000};
        authority.onAcquisitionChange(live);
        (void)authority.service(true);
        const char marker = 'r';
        if(write(ready[1], &marker, 1) != 1)
            _exit(2);
        char reply;
        if(read(resume[0], &reply, 1) != 1)
            _exit(3);
        auto preserved = authority.sample(live, false);
        if(!preserved.ok() || preserved->remaining_ns < 50000000)
            _exit(4);
        auto renewal = authority.sample(live, true);
        if(!renewal.ok())
            _exit(5);
        for(int i = 0; i < 70; ++i)
        {
            std::this_thread::sleep_for(2ms);
            (void)authority.service(true);
        }
        auto due = authority.sample(live, false);
        if(!due.ok() || due->remaining_ns != 0)
            _exit(6);
        _exit(0);
    }
    close(ready[1]);
    close(resume[0]);
    char marker;
    ASSERT_EQ(read(ready[0], &marker, 1), 1);
    ASSERT_EQ(kill(child, SIGSTOP), 0);
    int status{};
    ASSERT_EQ(waitpid(child, &status, WUNTRACED), child);
    ASSERT_TRUE(WIFSTOPPED(status));
    std::this_thread::sleep_for(200ms);
    ASSERT_EQ(kill(child, SIGCONT), 0);
    ASSERT_EQ(write(resume[1], &marker, 1), 1);
    ASSERT_EQ(waitpid(child, &status, 0), child);
    close(ready[0]);
    close(resume[1]);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(CatalogLeaseTest, TypedCausesSurviveWireConversion)
{
    for(auto cause: {AcquisitionTerminationCause::Released,
                     AcquisitionTerminationCause::Superseded,
                     AcquisitionTerminationCause::OwnerRemoved})
    {
        AcquisitionChange terminal{8, 1, 2, 3, {"keeper", "keeper:1"}, AcquisitionState::Released, 0, cause};
        EXPECT_EQ(convert::toProto(terminal).termination_cause(), static_cast<v1::AcquisitionTerminationCause>(cause));
        auto status = terminalRetry(3, cause);
        v1::AcquireResponse response;
        *response.mutable_status() = convert::toProto(status);
        convert::acquireRefusal(status, response);
        auto restored = getAcquireRefusal(convert::acquireStatus(response));
        ASSERT_TRUE(restored);
        EXPECT_EQ(restored->termination_cause, cause);
        EXPECT_EQ(restored->matched_incarnation, 3u);
    }
}
} // namespace chronolog::visor
