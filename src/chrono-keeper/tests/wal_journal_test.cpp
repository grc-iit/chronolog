#include <gtest/gtest.h>

#include <fstream>
#include <future>

#include "adapter/JournalService.h"
#include "wal_harness.h"

namespace chronolog::test
{
namespace
{
using namespace std::chrono_literals;

AppendBatch batch(std::initializer_list<uint64_t> sequences)
{
    AppendBatch out{1, 7, {}};
    for(auto sequence: sequences)
    {
        AppendItem item;
        item.writer_id = 2;
        item.incarnation = 3;
        item.sequence = sequence;
        item.envelope.payload = "event " + std::to_string(sequence);
        out.items.push_back(std::move(item));
    }
    return out;
}
Range all() { return {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}}; }

TEST(WalJournal, TornTailRecoveryPreservesDurableEvents)
{
    WalRig rig;
    auto appended = rig.current->append(batch({1, 2}), Durability::Durable);
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE((*appended)[1].status.ok());
    rig.journal.reset();
    const auto path = std::filesystem::path(rig.control->directory) / "1.wal";
    Event tail;
    tail.id = {1, 2, 3, 3};
    tail.hlc = {100, 999};
    auto bytes = wal::frame(wal::encode(tail));
    bytes.resize(bytes.size() - 2);
    std::ofstream(path, std::ios::binary | std::ios::app).write(bytes.data(), bytes.size());
    rig.reopen();
    auto events = rig.current->read(1, all());
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 2u);
    for(size_t i = 0; i < events->size(); ++i) EXPECT_EQ((*events)[i].hlc, (*appended)[i].hlc);
    auto next = rig.current->append(batch({3}), Durability::Durable);
    ASSERT_TRUE(next.ok());
    EXPECT_TRUE((*next)[0].status.ok());
}

TEST(WalJournal, FsyncFailureFailsTheGroup)
{
    WalRig rig;
    rig.control->block();
    {
        std::lock_guard lock(rig.control->mu);
        rig.control->fail = true;
    }
    auto future =
            std::async(std::launch::async, [&] { return rig.current->append(batch({1, 2, 3}), Durability::Durable); });
    (void)rig.control->waitPending();
    rig.control->release();
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    auto results = future.get();
    ASSERT_TRUE(results.ok());
    ASSERT_EQ(results->size(), 3u);
    for(const auto& result: *results)
    {
        EXPECT_EQ(result.status.code(), absl::StatusCode::kUnavailable);
        EXPECT_EQ(result.achieved, Durability::Unspecified);
    }
    {
        std::lock_guard lock(rig.control->mu);
        rig.control->fail = false;
    }
    auto failed = rig.current->append(batch({4}), Durability::Durable);
    ASSERT_TRUE(failed.ok());
    EXPECT_EQ((*failed)[0].status.code(), absl::StatusCode::kUnavailable);
    auto accepted = rig.current->append(batch({4}), Durability::Accepted);
    ASSERT_TRUE(accepted.ok());
    EXPECT_TRUE((*accepted)[0].status.ok());
    auto events = rig.current->read(1, all());
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 1u);
    EXPECT_EQ(events->front().id.sequence, 4u);
}

class ScanJournal final: public WalJournal
{
public:
    using WalJournal::WalJournal;
    mutable std::promise<void> scanned;
    std::shared_future<void> resume;

protected:
    void writerScanned(WriterKey) const override
    {
        if(resume.valid())
        {
            scanned.set_value();
            if(resume.wait_for(5s) != std::future_status::ready)
                throw std::runtime_error("scan did not resume");
        }
    }
};

TEST(WalJournal, FsyncCompletesBetweenScanAndCapDoesNotLoseEvent)
{
    WalRig rig;
    rig.journal.reset();
    ScanJournal journal(rig.clock, rig.membership, rig.ram_config, rig.config, rig.factory());
    ASSERT_TRUE(journal.registerWriter(1, 2, 3).ok());
    rig.control->block();
    auto append = std::async(std::launch::async, [&] { return journal.append(batch({1}), Durability::Durable); });
    const auto pending = rig.control->waitPending();
    std::promise<void> resume;
    journal.resume = resume.get_future().share();
    auto scanned = journal.scanned.get_future();
    auto scan = std::async(std::launch::async, [&] { return journal.sealedRead(1, all()); });
    EXPECT_EQ(scanned.wait_for(5s), std::future_status::ready);
    rig.control->release();
    EXPECT_EQ(append.wait_for(5s), std::future_status::ready);
    auto result = append.get();
    EXPECT_TRUE(result.ok());
    resume.set_value();
    auto snapshot = scan.get();
    ASSERT_TRUE(snapshot.ok());
    EXPECT_TRUE(snapshot->events.empty());
    EXPECT_LE(snapshot->view.sealed, pending);
    journal.resume = {};
    auto next = journal.sealedRead(1, all());
    ASSERT_TRUE(next.ok());
    ASSERT_EQ(next->events.size(), 1u);
    EXPECT_GT(next->view.sealed, pending);
}

TEST(WalJournal, DurableRpcDoesNotOccupyTheWorkerDuringFsync)
{
    WalRig rig;
    keeper::WorkerPool pool(1, 8);
    keeper::JournalService service(*rig.current, pool);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    auto stub = v1::Journal::NewStub(
            grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    auto call = [&](Durability durability, uint64_t sequence)
    {
        v1::AppendRequest request;
        request.set_story_id(1);
        request.set_epoch(7);
        request.set_durability(durability == Durability::Durable ? v1::DURABILITY_DURABLE : v1::DURABILITY_ACCEPTED);
        auto* item = request.add_items();
        item->set_writer_id(2);
        item->set_incarnation(3);
        item->set_sequence(sequence);
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        v1::AppendResponse response;
        EXPECT_TRUE(stub->Append(&context, request, &response).ok());
        return response;
    };
    rig.control->block();
    auto durable = std::async(std::launch::async, [&] { return call(Durability::Durable, 1); });
    (void)rig.control->waitPending();
    auto accepted = call(Durability::Accepted, 2);
    rig.control->release();
    EXPECT_EQ(accepted.results_size(), 1);
    ASSERT_EQ(durable.wait_for(5s), std::future_status::ready);
    auto response = durable.get();
    ASSERT_EQ(response.results_size(), 1);
    EXPECT_EQ(response.results(0).achieved_durability(), v1::DURABILITY_DURABLE);
    server->Shutdown(std::chrono::system_clock::now() + 2s);
}

} // namespace
} // namespace chronolog::test
