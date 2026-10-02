#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>

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

// Sends the scripted snapshot, then whatever the test pushes, until the client cancels.
class FakeCluster final: public iv1::Cluster::Service
{
public:
    void script(std::vector<iv1::WatchRoutesResponse> snapshot)
    {
        std::lock_guard lock(mutex_);
        snapshot_ = std::move(snapshot);
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
        {
            std::lock_guard lock(mutex_);
            snapshot = snapshot_;
        }
        for(const auto& message: snapshot)
            if(!writer->Write(message))
                return grpc::Status::OK;
        // Bounded: ends when the client cancels or 20 s pass.
        for(int tick = 0; tick < 400 && !context->IsCancelled(); ++tick)
        {
            std::unique_lock lock(mutex_);
            ready_.wait_for(lock, 50ms, [this] { return !outbox_.empty(); });
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
        rig_.journal->onDrop([this](StoryId story) { dropped_.set_value(story); });
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
                                                          std::move(lookup),
                                                          0ms);
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
    std::promise<StoryId> dropped_;
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<keeper::RouteWatcher> watcher_;
};

} // namespace

TEST_F(RouteWatcherTest, TombstoneUpdateIsAppliedWithoutARoute)
{
    cluster_.script({RouteUpdate(1, 5, 7)});
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
    EXPECT_EQ(membership_.route(1)->epoch, 7u);
}

TEST_F(RouteWatcherTest, SnapshotReconciliationConfirmsAStoryTheSnapshotDoesNotList)
{
    // The journal holds story 1 only, and the snapshot lists story 3.
    cluster_.script({RouteUpdate(3, 5, 7)});
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

} // namespace chronolog
