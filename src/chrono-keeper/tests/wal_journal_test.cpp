#include <gtest/gtest.h>

#include <algorithm>
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

namespace chronolog::test
{
TEST(WalJournal, AcceptedThenDurableUpgradeSurvivesCrashWithOutOfOrderSequences)
{
    WalRig rig;
    AppendBatch batch{1, 7, {}};
    for(uint64_t sequence = 1; sequence <= 3; ++sequence)
    {
        AppendItem item;
        item.writer_id = 2;
        item.incarnation = 3;
        item.sequence = sequence;
        item.envelope.payload = "original";
        batch.items.push_back(item);
    }
    auto accepted = rig.current->append(batch, Durability::Accepted);
    ASSERT_TRUE(accepted.ok());
    for(int index: {2, 0, 1})
    {
        auto item = batch.items[index];
        item.envelope.payload = "retry must not replace payload";
        auto durable = rig.current->append({1, 7, {item}}, index == 0 ? Durability::Unspecified : Durability::Durable);
        ASSERT_TRUE(durable.ok());
        ASSERT_TRUE(durable->front().status.ok());
        EXPECT_EQ(durable->front().hlc, (*accepted)[index].hlc);
        EXPECT_EQ(durable->front().achieved, Durability::Durable);
    }
    rig.reopen();
    auto replay = rig.current->append(batch, Durability::Durable);
    ASSERT_TRUE(replay.ok());
    for(size_t i = 0; i < replay->size(); ++i)
    {
        EXPECT_TRUE((*replay)[i].status.ok());
        EXPECT_EQ((*replay)[i].hlc, (*accepted)[i].hlc);
    }
    auto events = rig.current->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 3u);
    for(const auto& event: *events) EXPECT_EQ(event.envelope.payload, "original");
    auto item = batch.items.front();
    item.sequence = 4;
    auto next = rig.current->append({1, 7, {item}}, Durability::Durable);
    ASSERT_TRUE(next.ok());
    EXPECT_TRUE(next->front().status.ok());
}
} // namespace chronolog::test

namespace chronolog::test
{
TEST(WalJournal, PendingUpgradeKeepsTheOriginalVisibleAndMakesRetriesWait)
{
    WalRig rig;
    AppendItem item;
    item.writer_id = 2;
    item.incarnation = 3;
    item.sequence = 1;
    item.envelope.payload = "original";
    auto original = rig.current->append({1, 7, {item}}, Durability::Accepted);
    ASSERT_TRUE(original.ok());
    ASSERT_TRUE(original->front().status.ok());
    rig.control->block();
    auto upgrade =
            std::async(std::launch::async, [&] { return rig.current->append({1, 7, {item}}, Durability::Durable); });
    (void)rig.control->waitPending();
    auto retry =
            std::async(std::launch::async, [&] { return rig.current->append({1, 7, {item}}, Durability::Accepted); });
    EXPECT_EQ(retry.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    auto events = rig.current->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    EXPECT_TRUE(events.ok());
    if(events.ok() && events->size() == 1)
    {
        EXPECT_EQ(events->front().durability, Durability::Accepted);
    }
    rig.control->release();
    ASSERT_EQ(upgrade.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(retry.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    for(auto* future: {&upgrade, &retry})
    {
        auto result = future->get();
        ASSERT_TRUE(result.ok());
        ASSERT_TRUE(result->front().status.ok());
        EXPECT_EQ(result->front().hlc, original->front().hlc);
        EXPECT_EQ(result->front().achieved, Durability::Durable);
    }
    events = rig.current->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 1u);
    EXPECT_EQ(events->front().durability, Durability::Durable);
}
} // namespace chronolog::test

namespace chronolog::test
{
TEST(WalJournal, RotationPreservesReleasedWritersAndUnsettledSealIdentity)
{
    WalRig rig;
    rig.config.wal_segment_bytes = 1;
    rig.reopen();
    auto appended = rig.current->append(batch({1}), Durability::Durable);
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE(appended->front().status.ok());
    Chunk chunk;
    chunk.id = "keeper:1:0.0";
    chunk.story_id = 1;
    chunk.start = {};
    chunk.end = {1000, 0};
    ASSERT_TRUE(rig.current->recordSeal(chunk).ok());
    rig.current->releaseWriter(1, 2, 3);
    ASSERT_TRUE(rig.current->recordSettled(chunk.id).ok());
    ASSERT_TRUE(rig.current->registerWriter(1, 4, 1).ok());
    rig.clock->setPhysical(1100);
    auto next_batch = batch({1});
    next_batch.items.front().writer_id = 4;
    next_batch.items.front().incarnation = 1;
    auto next = rig.current->append(next_batch, Durability::Durable);
    ASSERT_TRUE(next.ok());
    ASSERT_TRUE(next->front().status.ok());
    chunk.id = "keeper:1:1000.0";
    chunk.start = chunk.end;
    chunk.end = {2'000'000'000, 0};
    ASSERT_TRUE(rig.current->recordSeal(chunk).ok());
    rig.reopen();
    auto released = rig.current->append(batch({2}), Durability::Durable);
    ASSERT_TRUE(released.ok());
    EXPECT_EQ(released->front().status.code(), absl::StatusCode::kFailedPrecondition);
    const auto seals = rig.current->sealedChunks();
    ASSERT_EQ(seals.size(), 2u);
    auto seal = std::find_if(seals.begin(), seals.end(), [&](const auto& value) { return value.chunk.id == chunk.id; });
    ASSERT_NE(seal, seals.end());
    EXPECT_FALSE(seal->settled);
    EXPECT_EQ(seal->chunk.start, chunk.start);
    EXPECT_EQ(seal->chunk.end, chunk.end);
    EXPECT_GT(rig.clock->tick(), next->front().hlc);
}
} // namespace chronolog::test
