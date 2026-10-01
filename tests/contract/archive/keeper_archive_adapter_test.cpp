#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <future>
#include <thread>
#include <set>
#include "adapter_rig.h"
#include "adapter/Convert.h"
namespace chronolog
{
namespace
{
internal::v1::FetchHotRequest request()
{
    internal::v1::FetchHotRequest value;
    value.set_story_id(1);
    value.mutable_hlc()->mutable_end()->set_physical_ns(INT64_MAX);
    return value;
}
} // namespace
TEST(ArchiveTransferTest, FetchHotTicksFrontierBeforeScan)
{
    test::AdapterRig rig;
    AppendItem item;
    item.writer_id = 2;
    item.incarnation = 3;
    item.sequence = 1;
    auto first = rig.rig.journal->append({1, 7, {item}}, Durability::Accepted);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first->front().status.code(), absl::StatusCode::kOk);
    item.sequence = 2;
    std::promise<Hlc> assigned;
    rig.rig.journal->onScanned(
            [&]
            {
                auto append = rig.rig.journal->append({1, 7, {item}}, Durability::Accepted);
                EXPECT_TRUE(append.ok());
                if(append.ok())
                {
                    EXPECT_EQ(append->front().status.code(), absl::StatusCode::kOk);
                    assigned.set_value(append->front().hlc);
                }
            });
    auto context = test::AdapterRig::context();
    auto stream = rig.archive->FetchHot(context.get(), request());
    internal::v1::FetchHotResponse message;
    std::vector<v1::Event> events;
    std::optional<Hlc> frontier;
    while(stream->Read(&message))
    {
        if(message.has_batch())
            for(const auto& event: message.batch().events()) events.push_back(event);
        if(message.has_trailer())
            frontier = keeper::convert::fromProto(message.trailer().sealed_frontier());
    }
    ASSERT_EQ(stream->Finish().error_code(), grpc::StatusCode::OK);
    rig.rig.journal->onScanned({});
    auto assigned_future = assigned.get_future();
    ASSERT_EQ(assigned_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto late = assigned_future.get();
    ASSERT_TRUE(frontier);
    EXPECT_LT(*frontier, late);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events.front().id().sequence(), 1u);
    EXPECT_LT(first->front().hlc, *frontier);
}
TEST(ArchiveTransferTest, FrontierTickOrderedBeforeInsert)
{
    test::AdapterRig rig;
    constexpr unsigned writers = 4, appends = 64;
    for(unsigned i = 0; i < writers; ++i)
        ASSERT_EQ(rig.rig.journal->registerWriter(1, 10 + i, 1).code(), absl::StatusCode::kOk);
    std::barrier start(writers + 1);
    std::atomic<unsigned> failures{};
    std::vector<std::thread> threads;
    for(unsigned i = 0; i < writers; ++i)
        threads.emplace_back(
                [&, i]
                {
                    start.arrive_and_wait();
                    for(unsigned seq = 1; seq <= appends; ++seq)
                    {
                        v1::AppendRequest append;
                        test::AdapterRig::fillRequest(append, 7, {seq});
                        append.mutable_items(0)->set_writer_id(10 + i);
                        append.mutable_items(0)->set_incarnation(1);
                        auto context = test::AdapterRig::context();
                        v1::AppendResponse response;
                        auto status = rig.journal->Append(context.get(), append, &response);
                        if(status.error_code() != grpc::StatusCode::OK || response.results_size() != 1 ||
                           response.results(0).status().code() != 0)
                            ++failures;
                    }
                });
    start.arrive_and_wait();
    for(unsigned i = 0; i < 32; ++i)
    {
        auto context = test::AdapterRig::context();
        auto stream = rig.archive->FetchHot(context.get(), request());
        internal::v1::FetchHotResponse message;
        std::set<std::pair<uint64_t, uint64_t>> ids;
        std::optional<Hlc> frontier;
        while(stream->Read(&message))
        {
            if(message.has_batch())
                for(const auto& event: message.batch().events())
                    ids.emplace(event.id().writer_id(), event.id().sequence());
            if(message.has_trailer())
                frontier = keeper::convert::fromProto(message.trailer().sealed_frontier());
        }
        EXPECT_EQ(stream->Finish().error_code(), grpc::StatusCode::OK);
        EXPECT_TRUE(frontier);
        if(!frontier)
            continue;
        auto below = rig.rig.journal->read(1, {Range::Axis::Hlc, {}, *frontier});
        EXPECT_TRUE(below.ok());
        if(below.ok())
        {
            for(const auto& event: *below) EXPECT_TRUE(ids.contains({event.id.writer_id, event.id.sequence}));
        }
    }
    for(auto& thread: threads) thread.join();
    EXPECT_EQ(failures.load(), 0u);
    auto all = rig.rig.journal->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all->size(), writers * appends);
}
} // namespace chronolog
