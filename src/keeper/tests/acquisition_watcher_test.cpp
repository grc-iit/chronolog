#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "keeper/membership/AcquisitionWatcher.h"
#include "keeper/tests/ram_harness.h"

namespace chronolog
{
namespace
{

using namespace std::chrono_literals;
namespace iv1 = chronolog::internal::v1;

AppendItem Item(uint64_t sequence)
{
    AppendItem i;
    i.writer_id = 2;
    i.incarnation = 3;
    i.sequence = sequence;
    i.physical = {100, 1, ClockStatus::Synced};
    i.envelope.payload = "event";
    return i;
}

iv1::AcquisitionUpdate Update(uint64_t revision, iv1::AcquisitionState state, const std::string& keeper = "self")
{
    iv1::AcquisitionUpdate u;
    u.set_revision(revision);
    u.set_story_id(1);
    u.set_writer_id(2);
    u.set_incarnation(3);
    u.mutable_assigned_keeper()->set_process_id(keeper);
    u.set_state(state);
    return u;
}

// Streams scripted messages. Connection n sends snapshots_[n] first (the last one repeats),
// then whatever the test pushes, until the test asks it to drop or the client goes away.
class FakeCluster final: public iv1::Cluster::Service
{
public:
    void script(std::vector<iv1::AcquisitionSnapshot> snapshots)
    {
        std::lock_guard lock(mutex_);
        snapshots_ = std::move(snapshots);
    }

    void push(iv1::AcquisitionUpdate update)
    {
        std::lock_guard lock(mutex_);
        outbox_.push_back(std::move(update));
    }

    void drop()
    {
        std::lock_guard lock(mutex_);
        drop_ = true;
    }

    // Connections from index n on wait before sending their snapshot, until release().
    void holdFrom(int n) { hold_from_ = n; }

    void release() { hold_from_ = INT_MAX; }

    int connections() const { return connections_; }

    grpc::Status WatchAcquisitions(grpc::ServerContext* context,
                                   const iv1::WatchAcquisitionsRequest* request,
                                   grpc::ServerWriter<iv1::WatchAcquisitionsResponse>* writer) override
    {
        if(request->keeper_id() != "self")
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "unexpected keeper id");
        size_t n = static_cast<size_t>(connections_++);
        for(int tick = 0; tick < 1000 && static_cast<int>(n) >= hold_from_ && !context->IsCancelled(); ++tick)
            std::this_thread::sleep_for(20ms);
        iv1::WatchAcquisitionsResponse first;
        {
            std::lock_guard lock(mutex_);
            *first.mutable_snapshot() = snapshots_[std::min(n, snapshots_.size() - 1)];
        }
        if(!writer->Write(first))
            return grpc::Status::OK;
        // Bounded: ends when the client cancels, a drop is requested or 20 s pass.
        for(int tick = 0; tick < 1000 && !context->IsCancelled(); ++tick)
        {
            std::optional<iv1::AcquisitionUpdate> next;
            bool dropping = false;
            {
                std::lock_guard lock(mutex_);
                if(drop_)
                {
                    drop_ = false;
                    dropping = true;
                }
                else if(!outbox_.empty())
                {
                    next = outbox_.front();
                    outbox_.pop_front();
                }
            }
            if(dropping)
                return grpc::Status::OK;
            if(next)
            {
                iv1::WatchAcquisitionsResponse message;
                *message.mutable_update() = *next;
                if(!writer->Write(message))
                    break;
            }
            else
            {
                std::this_thread::sleep_for(20ms);
            }
        }
        return grpc::Status::OK;
    }

private:
    std::mutex mutex_;
    std::vector<iv1::AcquisitionSnapshot> snapshots_;
    std::deque<iv1::AcquisitionUpdate> outbox_;
    bool drop_{};
    std::atomic<int> connections_{0};
    std::atomic<int> hold_from_{INT_MAX};
};

class AcquisitionWatcherTest: public ::testing::Test
{
protected:
    // The journal starts with no registered writer: admission comes only from the stream.
    AcquisitionWatcherTest()
        : journal_(clock_, membership_)
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&cluster_);
        server_ = builder.BuildAndStart();
        channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    }

    ~AcquisitionWatcherTest() override
    {
        watcher_.reset();
        server_->Shutdown(std::chrono::system_clock::now() + 2s);
    }

    void startWatcher()
    {
        watcher_ = std::make_unique<keeper::AcquisitionWatcher>(journal_,
                                                                "self",
                                                                [this]
                                                                {
                                                                    {
                                                                        std::lock_guard lock(fence_mutex_);
                                                                        ++fences_;
                                                                    }
                                                                    fence_cv_.notify_all();
                                                                });
        watcher_->start(channel_);
    }

    bool waitForFence()
    {
        // The hook follows advance so its W10.6 heartbeat carries the applied revision.
        // waitApplied can return before that hook runs.
        std::unique_lock lock(fence_mutex_);
        return fence_cv_.wait_for(lock, 5s, [this] { return fences_.load() >= 1; });
    }

    AppendResult append(uint64_t sequence)
    {
        auto r = journal_.append({1, 7, {Item(sequence)}}, Durability::Accepted);
        EXPECT_TRUE(r.ok());
        return (*r)[0];
    }

    static iv1::AcquisitionSnapshot Snapshot(uint64_t revision, std::vector<iv1::AcquisitionUpdate> active)
    {
        iv1::AcquisitionSnapshot s;
        s.set_revision(revision);
        for(auto& u: active) *s.add_acquisitions() = std::move(u);
        return s;
    }

    std::shared_ptr<FakeClock> clock_ = []
    {
        auto c = std::make_shared<FakeClock>(100);
        c->setStatus(ClockStatus::Synced);
        return c;
    }();
    std::shared_ptr<test::FakeMembership> membership_ = std::make_shared<test::FakeMembership>();
    RamJournal journal_;
    FakeCluster cluster_;
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::atomic<int> fences_{0};
    std::mutex fence_mutex_;
    std::condition_variable fence_cv_;
    std::unique_ptr<keeper::AcquisitionWatcher> watcher_;
};

} // namespace

TEST_F(AcquisitionWatcherTest, AppendAcceptedBetweenAcquiredAndReleased)
{
    cluster_.script({Snapshot(5, {Update(5, iv1::ACQUISITION_STATE_ACQUIRED)})});
    EXPECT_EQ(append(1).status.code(), absl::StatusCode::kFailedPrecondition);
    startWatcher();
    ASSERT_TRUE(watcher_->waitApplied(5, 5s));
    EXPECT_TRUE(append(1).status.ok());

    cluster_.push(Update(6, iv1::ACQUISITION_STATE_RELEASED));
    ASSERT_TRUE(watcher_->waitApplied(6, 5s));
    EXPECT_EQ(append(2).status.code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE(waitForFence());
    EXPECT_EQ(watcher_->appliedRevision(), 6u);
}

TEST_F(AcquisitionWatcherTest, ReconnectSnapshotFencesWriterItNoLongerLists)
{
    cluster_.script({Snapshot(5, {Update(5, iv1::ACQUISITION_STATE_ACQUIRED)}), Snapshot(9, {})});
    startWatcher();
    ASSERT_TRUE(watcher_->waitApplied(5, 5s));
    EXPECT_TRUE(append(1).status.ok());

    cluster_.drop();
    ASSERT_TRUE(watcher_->waitApplied(9, 10s));
    EXPECT_EQ(cluster_.connections(), 2);
    EXPECT_EQ(append(2).status.code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE(waitForFence());
}

TEST_F(AcquisitionWatcherTest, RegressedSnapshotEndsTheSessionAndAdmissionReopensAfterACurrentSnapshot)
{
    // The Keeper has applied revision 9. The first stream offers revision 7, which lists no writer.
    cluster_.script({Snapshot(7, {}), Snapshot(11, {Update(11, iv1::ACQUISITION_STATE_ACQUIRED)})});
    cluster_.holdFrom(1);
    watcher_ = std::make_unique<keeper::AcquisitionWatcher>(journal_, "self", [this] { ++fences_; });
    watcher_->applySnapshot(Snapshot(9, {Update(9, iv1::ACQUISITION_STATE_ACQUIRED)}));
    watcher_->applySnapshot(Snapshot(7, {}));
    ASSERT_EQ(watcher_->appliedRevision(), 9u);
    EXPECT_TRUE(append(1).status.ok());
    watcher_->start(channel_);

    // Rejecting it ends the stream, so the Watcher subscribes again. Admission stays closed and the writer is
    // not fenced while that second stream waits.
    const auto until = std::chrono::steady_clock::now() + 10s;
    while(cluster_.connections() < 2 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(10ms);
    ASSERT_EQ(cluster_.connections(), 2);
    EXPECT_EQ(journal_.append({1, 7, {Item(2)}}, Durability::Accepted).status().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(watcher_->appliedRevision(), 9u);
    EXPECT_EQ(fences_.load(), 0);

    cluster_.release();
    ASSERT_TRUE(watcher_->waitApplied(11, 10s));
    EXPECT_TRUE(append(2).status.ok());
    EXPECT_EQ(cluster_.connections(), 2);
    EXPECT_EQ(fences_.load(), 0);
}

TEST_F(AcquisitionWatcherTest, WriterAssignedElsewhereIsRejectedWithRoute)
{
    keeper::AcquisitionWatcher watcher(journal_, "self");
    watcher.applySnapshot(Snapshot(0, {}));
    watcher.applyUpdate(Update(1, iv1::ACQUISITION_STATE_ACQUIRED));
    EXPECT_TRUE(append(1).status.ok());
    watcher.applyUpdate(Update(2, iv1::ACQUISITION_STATE_ACQUIRED, "other"));
    auto r = append(2);
    EXPECT_EQ(r.status.code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE(r.current_route);
}

} // namespace chronolog
