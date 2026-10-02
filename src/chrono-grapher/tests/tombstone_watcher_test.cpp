#include "chrono-grapher/server/ArchiveService.h"
#include "chrono-grapher/server/TombstoneWatcher.h"
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <unistd.h>

namespace chronolog::grapher
{
namespace
{
using namespace std::chrono_literals;
namespace wire = internal::v1;

wire::WatchRoutesResponse Route(StoryId story, uint64_t revision)
{
    wire::WatchRoutesResponse message;
    message.set_story_id(story);
    message.set_revision(revision);
    message.mutable_route()->set_epoch(1);
    return message;
}

wire::WatchRoutesResponse Tombstone(StoryId story, uint64_t revision)
{
    wire::WatchRoutesResponse message;
    message.set_story_id(story);
    message.set_revision(revision);
    message.set_tombstoned(true);
    return message;
}

// Sends the scripted snapshot, then whatever the test pushes, until the client cancels.
class FakeCluster final: public wire::Cluster::Service
{
public:
    void script(std::vector<wire::WatchRoutesResponse> snapshot)
    {
        std::lock_guard lock(mutex_);
        snapshot_ = std::move(snapshot);
    }
    void push(wire::WatchRoutesResponse message)
    {
        {
            std::lock_guard lock(mutex_);
            outbox_.push_back(std::move(message));
        }
        ready_.notify_all();
    }
    grpc::Status WatchRoutes(grpc::ServerContext* context,
                             const wire::WatchRoutesRequest*,
                             grpc::ServerWriter<wire::WatchRoutesResponse>* writer) override
    {
        std::vector<wire::WatchRoutesResponse> snapshot;
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
    std::vector<wire::WatchRoutesResponse> snapshot_;
    std::deque<wire::WatchRoutesResponse> outbox_;
};

class TombstoneWatcherTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path() /
                ("chronolog_tombstone_" + std::to_string(::getpid()) + "_" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root_);
        std::map<StoryId, Hlc> anchors;
        for(StoryId story = 1; story <= 4; ++story) anchors[story] = {100, 0};
        auto opened = FileTierStore::Open(root_, "test-writer", anchors, std::make_shared<ProtoChunkCodec>());
        ASSERT_TRUE(opened.ok()) << opened.status();
        store_ = *std::move(opened);
        for(StoryId story = 1; story <= 4; ++story)
        {
            Event event;
            event.id = {story, 2, 3, 1};
            event.hlc = {150, 0};
            event.durability = Durability::Durable;
            event.envelope.payload = "payload";
            ASSERT_TRUE(store_->publish({"chunk", story, {100, 0}, {200, 0}, {event}, false}).ok());
        }
        archive_ = std::make_unique<ArchiveService>(*store_, "instance");
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&cluster_);
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    }

    void TearDown() override
    {
        watcher_.reset();
        archive_.reset();
        if(server_)
            server_->Shutdown(std::chrono::system_clock::now() + 2s);
        store_.reset();
        std::filesystem::remove_all(root_);
    }

    // Answers like the Catalog: `destroyed` stories are tombstoned and the rest are live. Calls are recorded.
    void start(std::set<StoryId> destroyed, int unavailable_first = 0)
    {
        watcher_ = std::make_unique<TombstoneWatcher>(
                *archive_,
                channel_,
                "grapher-1",
                "instance",
                [this, destroyed = std::move(destroyed), unavailable_first](StoryId story) -> absl::StatusOr<bool>
                {
                    std::lock_guard lock(lookups_mutex_);
                    const int call = ++lookups_[story];
                    lookups_changed_.notify_all();
                    if(call <= unavailable_first)
                        return absl::UnavailableError("catalog not reachable");
                    return destroyed.contains(story);
                },
                0ms);
    }

    bool waitLookups(StoryId story, int calls)
    {
        std::unique_lock lock(lookups_mutex_);
        return lookups_changed_.wait_for(lock, 10s, [&] { return lookups_[story] >= calls; });
    }

    ManifestState state(StoryId story) { return store_->manifest(story)->front().state; }

    std::filesystem::path root_;
    std::unique_ptr<FileTierStore> store_;
    std::unique_ptr<ArchiveService> archive_;
    FakeCluster cluster_;
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<TombstoneWatcher> watcher_;
    std::mutex lookups_mutex_;
    std::condition_variable lookups_changed_;
    std::map<StoryId, int> lookups_;
};

TEST_F(TombstoneWatcherTest, TombstonedUpdatesFreeEveryDestroyedStoryAndNothingElse)
{
    cluster_.script({Route(3, 5), Route(4, 5)});
    start({});
    // One chronicle destroy emits one tombstone per story at a single revision.
    cluster_.push(Tombstone(1, 6));
    cluster_.push(Tombstone(2, 6));
    ASSERT_TRUE(archive_->waitDestroyed(1, 10s));
    ASSERT_TRUE(archive_->waitDestroyed(2, 10s));
    EXPECT_EQ(state(1), ManifestState::Deleted);
    EXPECT_EQ(state(2), ManifestState::Deleted);
    EXPECT_EQ(state(3), ManifestState::Published);
    EXPECT_EQ(state(4), ManifestState::Published);
    EXPECT_TRUE(store_->tombstoned(2).value());
    EXPECT_FALSE(store_->tombstoned(3).value());
}

TEST_F(TombstoneWatcherTest, SnapshotReconciliationConfirmsAStoryTheSnapshotDoesNotList)
{
    cluster_.script({Route(2, 5), Route(3, 5)});
    // Story 1 was destroyed while this Grapher was down; the Catalog answers only on the third attempt. Story 4 is
    // absent from the snapshot too but live.
    start({1}, 2);
    ASSERT_TRUE(waitLookups(4, 3));
    ASSERT_TRUE(archive_->waitDestroyed(1, 10s));
    EXPECT_EQ(state(1), ManifestState::Deleted);
    EXPECT_EQ(state(2), ManifestState::Published);
    EXPECT_EQ(state(3), ManifestState::Published);
    EXPECT_EQ(state(4), ManifestState::Published) << "absence from a snapshot is never taken for a destroy";
    std::lock_guard lock(lookups_mutex_);
    EXPECT_GE(lookups_[1], 3);
}
} // namespace
} // namespace chronolog::grapher
