#include <gtest/gtest.h>
#include <future>
#include <thread>

#include "TestSupport.h"
#include "adapter/ClusterService.h"
#include "catalog/SqliteMetadataStore.h"
#include "adapter/JournalService.h"
#include "membership/AcquisitionWatcher.h"
#include "membership/ConfigMembership.h"
#include "clock/FakeClock.h"
#include "wal/WalJournal.h"

namespace chronolog
{
namespace
{
using namespace std::chrono_literals;
namespace iv1 = internal::v1;
struct AcquisitionRig
{
    visor::testing::TempDir directory;
    visor::Topology topology{{{"keeper-a", "keeper-a:1"}, {"keeper-b", "keeper-b:1"}}, "grapher:1", "player:1"};
    std::unique_ptr<visor::SqliteMetadataStore> store;
    visor::AcquisitionFeed feed;
    std::unique_ptr<visor::StaticRouteMembership> visor_membership;
    std::unique_ptr<visor::ClusterService> cluster;
    std::shared_ptr<keeper::ConfigMembership> membership;
    std::shared_ptr<FakeClock> clock;
    WalJournalConfig config;
    std::unique_ptr<WalJournal> journal;
    std::unique_ptr<keeper::AcquisitionWatcher> watcher;
    keeper::WorkerPool pool{2, 32};
    std::unique_ptr<keeper::JournalService> service;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<v1::Journal::Stub> stub;
    AcquisitionRig()
    {
        auto opened = visor::SqliteMetadataStore::open((directory.path() / "catalog.sqlite").string(), topology);
        if(!opened.ok())
            throw std::runtime_error(std::string(opened.status().message()));
        store = std::move(*opened);
        if(!store->createChronicle("c").ok() || !store->createStory("c", "s").ok())
            throw std::runtime_error("Catalog seed failed");
        store->setObserver(&feed);
        visor_membership =
                std::make_unique<visor::StaticRouteMembership>(topology, 1, [](StoryId id) { return id == 1; }, 15s);
        cluster = std::make_unique<visor::ClusterService>(*visor_membership, *store, *store, feed);
        membership =
                std::make_shared<keeper::ConfigMembership>(std::vector<keeper::StaticRoute>{{1, topology.routeFor(1)}});
        config.wal_dir = (directory.path() / "wal").string();
        openJournal();
        startServer();
    }
    ~AcquisitionRig()
    {
        watcher.reset();
        cluster->shutdown();
        server->Shutdown(std::chrono::system_clock::now() + 2s);
        store->setObserver(nullptr);
    }
    void openJournal()
    {
        clock = std::make_shared<FakeClock>(100);
        clock->setStatus(ClockStatus::Synced);
        journal = std::make_unique<WalJournal>(clock, membership, RamJournalConfig{}, config);
        watcher = std::make_unique<keeper::AcquisitionWatcher>(*journal, "keeper-b");
        service = std::make_unique<keeper::JournalService>(*journal, pool);
    }
    void startServer()
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(cluster.get());
        builder.RegisterService(service.get());
        server = builder.BuildAndStart();
        if(!server)
            throw std::runtime_error("Acquisition server did not start");
        channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        stub = v1::Journal::NewStub(channel);
    }
    void restart()
    {
        watcher.reset();
        server->Shutdown(std::chrono::system_clock::now() + 2s);
        server.reset();
        service.reset();
        journal.reset();
        openJournal();
        startServer();
    }
    std::pair<grpc::Status, v1::AppendResponse> append(uint64_t sequence, uint64_t incarnation = 1)
    {
        v1::AppendRequest request;
        request.set_story_id(1);
        request.set_epoch(1);
        request.set_durability(v1::DURABILITY_DURABLE);
        auto* item = request.add_items();
        item->set_writer_id(1);
        item->set_incarnation(incarnation);
        item->set_sequence(sequence);
        item->mutable_envelope()->set_payload("event");
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        v1::AppendResponse response;
        auto status = stub->Append(&context, request, &response);
        return {std::move(status), std::move(response)};
    }
};
} // namespace

TEST(AcquisitionWatcherTest, RevisionGapsAreSkipped)
{
    AcquisitionRig rig;
    auto first = rig.store->acquire(1, "first");
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first->assigned_keeper.process_id, "keeper-b");
    rig.watcher->start(rig.channel);
    ASSERT_TRUE(rig.watcher->waitApplied(1, 5s));
    auto accepted = rig.append(1);
    ASSERT_EQ(accepted.first.error_code(), grpc::StatusCode::OK);
    ASSERT_EQ(accepted.second.results_size(), 1);
    ASSERT_EQ(accepted.second.results(0).status().code(), 0);
    auto other = rig.store->acquire(1, "other");
    ASSERT_TRUE(other.ok());
    ASSERT_EQ(other->assigned_keeper.process_id, "keeper-a");
    EXPECT_FALSE(rig.watcher->waitApplied(2, 20ms));
    auto released = rig.store->release(1, first->writer_id, first->incarnation);
    ASSERT_TRUE(released.ok());
    ASSERT_EQ(released->revision, 3u);
    ASSERT_TRUE(rig.watcher->waitApplied(3, 5s));
    EXPECT_EQ(rig.watcher->appliedRevision(), 3u);
    auto fenced = rig.append(2);
    ASSERT_EQ(fenced.first.error_code(), grpc::StatusCode::OK);
    ASSERT_EQ(fenced.second.results_size(), 1);
    EXPECT_EQ(fenced.second.results(0).status().code(), static_cast<int>(absl::StatusCode::kFailedPrecondition));
}

TEST(AcquisitionWatcherTest, RestartRestoresFencesBeforeAdmission)
{
    AcquisitionRig rig;
    auto acquired = rig.store->acquire(1, "writer");
    ASSERT_TRUE(acquired.ok());
    rig.watcher->start(rig.channel);
    ASSERT_TRUE(rig.watcher->waitApplied(1, 5s));
    auto accepted = rig.append(1);
    ASSERT_EQ(accepted.first.error_code(), grpc::StatusCode::OK);
    ASSERT_EQ(accepted.second.results_size(), 1);
    ASSERT_EQ(accepted.second.results(0).status().code(), 0);
    auto original = accepted.second.results(0).assigned_hlc();
    rig.watcher.reset();
    auto released = rig.store->release(1, acquired->writer_id, acquired->incarnation);
    ASSERT_TRUE(released.ok());
    rig.restart();
    auto before = rig.append(2);
    EXPECT_EQ(before.first.error_code(), grpc::StatusCode::UNAVAILABLE);
    rig.watcher->start(rig.channel);
    ASSERT_TRUE(rig.watcher->waitApplied(released->revision, 5s));
    auto fenced = rig.append(2);
    ASSERT_EQ(fenced.first.error_code(), grpc::StatusCode::OK);
    ASSERT_EQ(fenced.second.results_size(), 1);
    EXPECT_EQ(fenced.second.results(0).status().code(), static_cast<int>(absl::StatusCode::kFailedPrecondition));
    auto events = rig.journal->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 1u);
    EXPECT_EQ(events->front().id.sequence, 1u);
    EXPECT_EQ(events->front().hlc, (Hlc{original.physical_ns(), original.logical()}));
    auto newer = rig.store->acquire(1, "writer");
    ASSERT_TRUE(newer.ok());
    auto snapshot = rig.store->snapshotAcquisitions();
    ASSERT_TRUE(snapshot.ok());
    ASSERT_TRUE(rig.watcher->waitApplied(snapshot->revision, 5s));
    auto admitted = rig.append(1, newer->incarnation);
    ASSERT_EQ(admitted.first.error_code(), grpc::StatusCode::OK);
    ASSERT_EQ(admitted.second.results_size(), 1);
    EXPECT_EQ(admitted.second.results(0).status().code(), 0);
}

TEST(AcquisitionWatcherTest, AppliedRevisionIsMonotonic)
{
    AcquisitionRig rig;
    auto acquired = rig.store->acquire(1, "writer");
    ASSERT_TRUE(acquired.ok());
    auto old = rig.store->snapshotAcquisitions();
    ASSERT_TRUE(old.ok());
    rig.watcher->start(rig.channel);
    ASSERT_TRUE(rig.watcher->waitApplied(old->revision, 5s));
    std::promise<void> reading;
    auto reader = std::async(std::launch::async,
                             [&]
                             {
                                 uint64_t previous = rig.watcher->appliedRevision();
                                 reading.set_value();
                                 for(unsigned i = 0; i < 512; ++i)
                                 {
                                     auto current = rig.watcher->appliedRevision();
                                     EXPECT_GE(current, previous);
                                     previous = current;
                                     std::this_thread::yield();
                                 }
                             });
    reading.get_future().wait();
    auto released = rig.store->release(1, acquired->writer_id, acquired->incarnation);
    ASSERT_TRUE(released.ok());
    ASSERT_TRUE(rig.watcher->waitApplied(released->revision, 5s));
    reader.get();
    iv1::AcquisitionSnapshot stale;
    stale.set_revision(old->revision);
    rig.watcher->applySnapshot(stale);
    iv1::AcquisitionUpdate duplicate;
    duplicate.set_revision(old->revision);
    duplicate.set_story_id(1);
    duplicate.set_writer_id(acquired->writer_id);
    duplicate.set_incarnation(acquired->incarnation);
    duplicate.set_state(iv1::ACQUISITION_STATE_RELEASED);
    rig.watcher->applyUpdate(duplicate);
    EXPECT_EQ(rig.watcher->appliedRevision(), released->revision);
    auto fenced = rig.append(2);
    ASSERT_EQ(fenced.first.error_code(), grpc::StatusCode::OK);
    ASSERT_EQ(fenced.second.results_size(), 1);
    EXPECT_EQ(fenced.second.results(0).status().code(), static_cast<int>(absl::StatusCode::kFailedPrecondition));
}
} // namespace chronolog
