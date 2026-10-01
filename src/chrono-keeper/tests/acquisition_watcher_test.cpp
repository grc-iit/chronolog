#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "membership/AcquisitionWatcher.h"
#include "ram_harness.h"

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

    int connections() const { return connections_; }

    grpc::Status WatchAcquisitions(grpc::ServerContext* context,
                                   const iv1::WatchAcquisitionsRequest* request,
                                   grpc::ServerWriter<iv1::WatchAcquisitionsResponse>* writer) override
    {
        if(request->keeper_id() != "self")
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "unexpected keeper id");
        size_t n = static_cast<size_t>(connections_++);
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
        watcher_ = std::make_unique<keeper::AcquisitionWatcher>(journal_, "self", [this] { ++fences_; });
        watcher_->start(channel_);
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
    EXPECT_GE(fences_.load(), 1);
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
    EXPECT_GE(fences_.load(), 1);
}

TEST_F(AcquisitionWatcherTest, WriterAssignedElsewhereIsRejectedWithRoute)
{
    keeper::AcquisitionWatcher watcher(journal_, "self");
    watcher.applyUpdate(Update(1, iv1::ACQUISITION_STATE_ACQUIRED));
    EXPECT_TRUE(append(1).status.ok());
    watcher.applyUpdate(Update(2, iv1::ACQUISITION_STATE_ACQUIRED, "other"));
    auto r = append(2);
    EXPECT_EQ(r.status.code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE(r.current_route);
}

TEST_F(AcquisitionWatcherTest, SnapshotRevisionNeverMovesBackwards)
{
    keeper::AcquisitionWatcher watcher(journal_, "self");
    watcher.applySnapshot(Snapshot(8, {}));
    watcher.applyUpdate(Update(7, iv1::ACQUISITION_STATE_RELEASED));
    EXPECT_EQ(watcher.appliedRevision(), 8u);
}

} // namespace chronolog
