#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>

#include "adapter/Convert.h"
#include "membership/RouteWatcher.h"
#include "ram_harness.h"

namespace chronolog
{
namespace
{

using namespace std::chrono_literals;
namespace iv1 = chronolog::internal::v1;

iv1::WatchRoutesResponse RouteUpdate(StoryId story, uint64_t revision, Epoch epoch)
{
    iv1::WatchRoutesResponse message;
    message.set_story_id(story);
    message.set_revision(revision);
    message.mutable_route()->set_epoch(epoch);
    message.mutable_route()->add_keepers()->set_process_id("self");
    message.mutable_route()->add_keepers()->set_process_id("other");
    return message;
}

iv1::WatchRoutesResponse Tombstone(StoryId story, uint64_t revision)
{
    iv1::WatchRoutesResponse message;
    message.set_story_id(story);
    message.set_revision(revision);
    message.set_tombstoned(true);
    return message;
}

iv1::WatchRoutesResponse SnapshotEnd(uint64_t revision)
{
    iv1::WatchRoutesResponse message;
    message.set_revision(revision);
    message.set_snapshot_end(true);
    return message;
}

// Each connect sends the scripted snapshot, waiting `pace` before every message, then whatever the test pushes
// until the client cancels or the test hangs up. With `close` the stream ends right after the snapshot.
class FakeCluster final: public iv1::Cluster::Service
{
public:
    void
    script(std::vector<iv1::WatchRoutesResponse> snapshot, std::chrono::milliseconds pace = 0ms, bool close = false)
    {
        std::lock_guard lock(mutex_);
        snapshot_ = std::move(snapshot);
        pace_ = pace;
        close_ = close;
    }
    void hangup()
    {
        {
            std::lock_guard lock(mutex_);
            ++generation_;
        }
        ready_.notify_all();
    }
    int connects()
    {
        std::lock_guard lock(mutex_);
        return connects_;
    }
    void push(iv1::WatchRoutesResponse message)
    {
        {
            std::lock_guard lock(mutex_);
            outbox_.push_back(std::move(message));
        }
        ready_.notify_all();
    }
    grpc::Status WatchRoutes(grpc::ServerContext* context,
                             const iv1::WatchRoutesRequest*,
                             grpc::ServerWriter<iv1::WatchRoutesResponse>* writer) override
    {
        std::vector<iv1::WatchRoutesResponse> snapshot;
        std::chrono::milliseconds pace;
        bool close;
        uint64_t generation;
        {
            std::lock_guard lock(mutex_);
            snapshot = snapshot_;
            pace = pace_;
            close = close_;
            generation = generation_;
            ++connects_;
        }
        for(const auto& message: snapshot)
        {
            std::this_thread::sleep_for(pace);
            if(!writer->Write(message))
                return grpc::Status::OK;
        }
        if(close)
            return grpc::Status::OK;
        // Bounded: ends when the client cancels, the test hangs up or 20 s pass.
        for(int tick = 0; tick < 400 && !context->IsCancelled(); ++tick)
        {
            std::unique_lock lock(mutex_);
            ready_.wait_for(lock, 50ms, [&] { return !outbox_.empty() || generation_ != generation; });
            if(generation_ != generation)
                break;
            if(outbox_.empty())
                continue;
            auto message = std::move(outbox_.front());
            outbox_.pop_front();
            lock.unlock();
            if(!writer->Write(message))
                break;
        }
        return grpc::Status::OK;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<iv1::WatchRoutesResponse> snapshot_;
    std::chrono::milliseconds pace_{};
    bool close_{};
    uint64_t generation_{};
    int connects_{};
    std::deque<iv1::WatchRoutesResponse> outbox_;
};

class RouteWatcherTest: public ::testing::Test
{
protected:
    RouteWatcherTest()
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&cluster_);
        server_ = builder.BuildAndStart();
        channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        rig_.journal->onDrop(
                [this](StoryId story)
                {
                    if(drops_++ == 0)
                        dropped_.set_value(story);
                });
    }

    ~RouteWatcherTest() override
    {
        watcher_.reset();
        server_->Shutdown(std::chrono::system_clock::now() + 2s);
    }

    void start(keeper::RouteWatcher::TombstoneLookup lookup = nullptr)
    {
        watcher_ = std::make_unique<keeper::RouteWatcher>(membership_,
                                                          channel_,
                                                          "self",
                                                          "instance",
                                                          rig_.journal,
                                                          std::move(lookup));
    }

    // The sentinel route is applied after everything pushed before it.
    void awaitApplied(StoryId story, Epoch epoch)
    {
        for(auto deadline = std::chrono::steady_clock::now() + 10s; std::chrono::steady_clock::now() < deadline;)
        {
            if(auto route = membership_.route(story); route.ok() && route->epoch == epoch)
                return;
            std::this_thread::yield();
        }
        FAIL() << "route " << story << " epoch " << epoch << " was never applied";
    }

    AppendResult append(uint64_t sequence)
    {
        AppendItem item;
        item.writer_id = 2;
        item.incarnation = 3;
        item.sequence = sequence;
        item.physical = {100, 1, ClockStatus::Synced};
        item.envelope.payload = "event";
        auto result = rig_.journal->append({1, 7, {item}}, Durability::Accepted);
        EXPECT_TRUE(result.ok());
        return (*result)[0];
    }

    test::RamRig rig_;
    keeper::ConfigMembership membership_;
    FakeCluster cluster_;
    std::atomic<int> drops_{0};
    std::promise<StoryId> dropped_;
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<keeper::RouteWatcher> watcher_;
};

} // namespace

TEST_F(RouteWatcherTest, TombstoneUpdateIsAppliedWithoutARoute)
{
    cluster_.script({RouteUpdate(1, 5, 7), SnapshotEnd(5)});
    start();
    awaitApplied(1, 7);
    EXPECT_TRUE(append(1).status.ok());

    // Below the applied revision and carrying no route: both guards would drop an ordinary update.
    cluster_.push(Tombstone(1, 2));
    auto dropped = dropped_.get_future();
    ASSERT_EQ(dropped.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(dropped.get(), 1u);
    EXPECT_TRUE(rig_.journal->dropped(1));
    EXPECT_EQ(append(2).status.code(), absl::StatusCode::kFailedPrecondition);

    // Later updates for the story are ignored, and the tombstone did not lower the applied revision.
    cluster_.push(RouteUpdate(1, 9, 8));
    cluster_.push(RouteUpdate(2, 4, 3));
    cluster_.push(RouteUpdate(2, 10, 4));
    awaitApplied(2, 4);
    EXPECT_EQ(membership_.route(1).status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST_F(RouteWatcherTest, SnapshotReconciliationConfirmsAStoryTheSnapshotDoesNotList)
{
    // The journal holds story 1 only, no route for it was learned, and the snapshot lists story 3.
    cluster_.script({RouteUpdate(3, 5, 7), SnapshotEnd(5)});
    std::mutex mutex;
    std::vector<StoryId> asked;
    std::atomic<int> failures{2};
    start(
            [&](StoryId story) -> absl::StatusOr<bool>
            {
                {
                    std::lock_guard lock(mutex);
                    asked.push_back(story);
                }
                if(failures-- > 0)
                    return absl::UnavailableError("catalog unavailable");
                return true;
            });
    auto dropped = dropped_.get_future();
    ASSERT_EQ(dropped.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(dropped.get(), 1u);
    std::lock_guard lock(mutex);
    EXPECT_EQ(asked, (std::vector<StoryId>{1, 1, 1}));
}

TEST_F(RouteWatcherTest, SlowSnapshotNeverLooksUpOrReconcilesBeforeItsMarker)
{
    std::atomic<int> lookups{0};
    cluster_.script({RouteUpdate(1, 5, 7), SnapshotEnd(5)}, 200ms);
    start(
            [&](StoryId) -> absl::StatusOr<bool>
            {
                ++lookups;
                return true;
            });
    awaitApplied(1, 7);
    EXPECT_TRUE(append(1).status.ok());

    // The reconnect snapshot lists story 1 last, long after the stream opened.
    cluster_.script({RouteUpdate(2, 9, 3), RouteUpdate(1, 9, 7), SnapshotEnd(9)}, 300ms);
    cluster_.hangup();
    cluster_.push(RouteUpdate(3, 10, 1));
    awaitApplied(3, 1);
    EXPECT_EQ(lookups.load(), 0);
    EXPECT_EQ(drops_.load(), 0);
    EXPECT_TRUE(append(2).status.ok());
}

TEST_F(RouteWatcherTest, DestroyDuringDisconnectIsAppliedOnceAtTheMarker)
{
    std::atomic<int> lookups{0};
    cluster_.script({RouteUpdate(1, 5, 7), SnapshotEnd(5)});
    start(
            [&](StoryId) -> absl::StatusOr<bool>
            {
                ++lookups;
                return absl::UnavailableError("catalog unavailable");
            });
    awaitApplied(1, 7);
    EXPECT_TRUE(append(1).status.ok());

    // Story 1 is destroyed at revision 6 while the Keeper is disconnected, so the next snapshot omits it.
    cluster_.script({RouteUpdate(2, 6, 3), SnapshotEnd(6)});
    cluster_.hangup();
    auto dropped = dropped_.get_future();
    ASSERT_EQ(dropped.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(dropped.get(), 1u);
    EXPECT_EQ(append(2).status.code(), absl::StatusCode::kFailedPrecondition);

    cluster_.hangup();
    cluster_.push(RouteUpdate(3, 7, 1));
    awaitApplied(3, 1);
    EXPECT_EQ(drops_.load(), 1);
    EXPECT_EQ(lookups.load(), 0);
}

TEST_F(RouteWatcherTest, StoryCreatedAfterTheSnapshotKeepsItsAppendsAtTheMarker)
{
    // Registration delivered story 1 at revision 6, above the snapshot cursor 5, and it already holds an event.
    auto state = keeper::convert::routeState(RouteUpdate(1, 6, 7));
    rig_.journal->applyRoute(1, state, false, 6, [&] { membership_.setRouteState(1, state); });
    EXPECT_TRUE(append(1).status.ok());

    std::atomic<int> lookups{0};
    std::promise<StoryId> asked;
    cluster_.script({RouteUpdate(2, 5, 3), SnapshotEnd(5)});
    start(
            [&](StoryId story) -> absl::StatusOr<bool>
            {
                if(lookups++ == 0)
                    asked.set_value(story);
                return false;
            });
    auto first = asked.get_future();
    ASSERT_EQ(first.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(first.get(), 1u);
    cluster_.push(RouteUpdate(3, 6, 1));
    awaitApplied(3, 1);
    EXPECT_EQ(lookups.load(), 1);
    EXPECT_EQ(drops_.load(), 0);
    EXPECT_TRUE(append(2).status.ok());
}

TEST_F(RouteWatcherTest, SnapshotBelowTheAppliedRevisionConcludesNothing)
{
    std::promise<StoryId> asked;
    std::atomic<int> lookups{0};
    cluster_.script({RouteUpdate(1, 8, 7), SnapshotEnd(8)});
    start(
            [&](StoryId story) -> absl::StatusOr<bool>
            {
                if(lookups++ == 0)
                    asked.set_value(story);
                return false;
            });
    awaitApplied(1, 7);

    // A lagging replica serves a snapshot at 5 that omits story 1; only the lookup may decide it.
    cluster_.script({RouteUpdate(2, 5, 3), SnapshotEnd(5)});
    cluster_.hangup();
    auto first = asked.get_future();
    ASSERT_EQ(first.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(first.get(), 1u);
    EXPECT_EQ(drops_.load(), 0);
}

TEST_F(RouteWatcherTest, StreamEndingBeforeItsMarkerReconcilesNothing)
{
    std::atomic<int> lookups{0};
    cluster_.script({RouteUpdate(1, 5, 7), SnapshotEnd(5)});
    start(
            [&](StoryId) -> absl::StatusOr<bool>
            {
                ++lookups;
                return true;
            });
    awaitApplied(1, 7);

    // Every later connect omits story 1 and ends before its marker.
    cluster_.script({RouteUpdate(2, 9, 3)}, 0ms, true);
    cluster_.hangup();
    awaitApplied(2, 3);
    for(auto deadline = std::chrono::steady_clock::now() + 10s;
        cluster_.connects() < 4 && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::yield();
    ASSERT_GE(cluster_.connects(), 4);
    EXPECT_EQ(lookups.load(), 0);
    EXPECT_EQ(drops_.load(), 0);
    EXPECT_FALSE(rig_.journal->dropped(1));
}

TEST_F(RouteWatcherTest, AcknowledgesTheSnapshotAtItsMarkerAndEachDeltaAfterIt)
{
    cluster_.script({RouteUpdate(1, 5, 7)});
    start();
    cluster_.push(RouteUpdate(2, 6, 3));
    awaitApplied(2, 3);
    EXPECT_EQ(rig_.journal->appliedRouteRevision(), 0u);

    cluster_.push(SnapshotEnd(6));
    cluster_.push(RouteUpdate(3, 7, 1));
    awaitApplied(3, 1);
    EXPECT_EQ(rig_.journal->appliedRouteRevision(), 7u);

    // A same-epoch change, such as a predecessor removed on settlement, reaches the Keeper only here.
    cluster_.push(RouteUpdate(1, 9, 7));
    for(auto deadline = std::chrono::steady_clock::now() + 10s;
        rig_.journal->appliedRouteRevision() < 9 && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::yield();
    EXPECT_EQ(rig_.journal->appliedRouteRevision(), 9u);
}

} // namespace chronolog
