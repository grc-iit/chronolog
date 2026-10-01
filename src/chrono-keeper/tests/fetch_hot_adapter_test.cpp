#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>

#include "adapter_rig.h"

namespace chronolog
{
namespace
{

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

struct Fetched
{
    grpc::Status status;
    std::vector<v1::Event> events;
    int trailers{};
    iv1::FetchHotTrailer trailer;
    bool trailer_last{};
};

Fetched FetchAll(iv1::Archive::Stub& stub, const iv1::FetchHotRequest& request)
{
    Fetched out;
    auto ctx = test::AdapterRig::context();
    auto reader = stub.FetchHot(ctx.get(), request);
    iv1::FetchHotResponse message;
    while(reader->Read(&message))
    {
        out.trailer_last = false;
        if(message.has_batch())
        {
            for(const auto& e: message.batch().events()) out.events.push_back(e);
        }
        else if(message.has_trailer())
        {
            ++out.trailers;
            out.trailer = message.trailer();
            out.trailer_last = true;
        }
    }
    out.status = reader->Finish();
    return out;
}

iv1::FetchHotRequest AllRequest(uint64_t max_events = 0)
{
    iv1::FetchHotRequest request;
    request.set_story_id(1);
    request.mutable_hlc()->mutable_end()->set_physical_ns(INT64_MAX);
    request.set_max_events(max_events);
    return request;
}

bool Before(const v1::Hlc& a, const v1::Hlc& b)
{
    return a.physical_ns() != b.physical_ns() ? a.physical_ns() < b.physical_ns() : a.logical() < b.logical();
}

} // namespace

TEST(ArchiveTransferTest, FetchHotStreamsBatchesThenOneTrailer)
{
    test::AdapterRig rig;
    ASSERT_TRUE(rig.rig.journal->registerWriter(1, 6, 1).ok());
    std::vector<AppendItem> items;
    for(uint64_t s = 1; s <= 5; ++s) items.push_back(Item(s));
    auto appended = rig.rig.journal->append({1, 7, items}, Durability::Accepted);
    ASSERT_TRUE(appended.ok());

    auto fetched = FetchAll(*rig.archive, AllRequest());
    ASSERT_TRUE(fetched.status.ok());
    EXPECT_EQ(fetched.events.size(), 5u);
    EXPECT_EQ(fetched.trailers, 1);
    EXPECT_TRUE(fetched.trailer_last);
    EXPECT_FALSE(fetched.trailer.truncated());
    EXPECT_EQ(fetched.trailer.epoch(), 7u);
    ASSERT_EQ(fetched.trailer.frontiers_size(), 2);
    for(const auto& e: fetched.events) EXPECT_TRUE(Before(e.hlc(), fetched.trailer.sealed_frontier()));
    for(const auto& f: fetched.trailer.frontiers())
        EXPECT_FALSE(Before(f.frontier(), fetched.trailer.sealed_frontier()) ||
                     Before(fetched.trailer.sealed_frontier(), f.frontier()));
    for(size_t i = 1; i < fetched.events.size(); ++i)
        EXPECT_TRUE(Before(fetched.events[i - 1].hlc(), fetched.events[i].hlc()));
}

TEST(ArchiveTransferTest, FetchHotTruncatesAtMaxEvents)
{
    test::AdapterRig rig;
    std::vector<AppendItem> items;
    for(uint64_t s = 1; s <= 5; ++s) items.push_back(Item(s));
    ASSERT_TRUE(rig.rig.journal->append({1, 7, items}, Durability::Accepted).ok());
    auto fetched = FetchAll(*rig.archive, AllRequest(2));
    ASSERT_TRUE(fetched.status.ok());
    EXPECT_EQ(fetched.events.size(), 2u);
    EXPECT_TRUE(fetched.trailer.truncated());
    EXPECT_EQ(fetched.trailers, 1);
}

TEST(ArchiveTransferTest, FetchHotRejectsBadRequests)
{
    test::AdapterRig rig;
    auto unknown = AllRequest();
    unknown.set_story_id(9);
    EXPECT_EQ(FetchAll(*rig.archive, unknown).status.error_code(), grpc::StatusCode::NOT_FOUND);

    iv1::FetchHotRequest no_range;
    no_range.set_story_id(1);
    EXPECT_EQ(FetchAll(*rig.archive, no_range).status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto backwards = AllRequest();
    backwards.mutable_hlc()->mutable_start()->set_physical_ns(10);
    backwards.mutable_hlc()->mutable_end()->set_physical_ns(5);
    EXPECT_EQ(FetchAll(*rig.archive, backwards).status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(ArchiveTransferTest, FetchHotOfWriterlessStoryStillCarriesSealedFrontier)
{
    test::AdapterRig rig;
    auto fetched = FetchAll(*rig.archive, AllRequest());
    ASSERT_TRUE(fetched.status.ok());
    EXPECT_TRUE(fetched.events.empty());
    EXPECT_GT(fetched.trailer.sealed_frontier().physical_ns(), 0);
}

// Appends race every FetchHot. Whatever the stream returned, every event the journal ends up
// holding below the trailer frontier must have been in that stream.
TEST(ArchiveTransferTest, FetchHotTicksFrontierBeforeScan)
{
    constexpr uint64_t kAppends = 400;
    constexpr int kFetches = 25;
    test::AdapterRig rig;
    std::atomic<uint64_t> failures{0};
    std::thread writer(
            [&]
            {
                for(uint64_t seq = 1; seq <= kAppends; ++seq)
                {
                    auto r = rig.rig.journal->append({1, 7, {Item(seq)}}, Durability::Accepted);
                    if(!r.ok() || !(*r)[0].status.ok())
                        ++failures;
                }
            });
    std::vector<Fetched> fetches;
    for(int i = 0; i < kFetches; ++i) fetches.push_back(FetchAll(*rig.archive, AllRequest()));
    writer.join();
    ASSERT_EQ(failures.load(), 0u);

    auto all = rig.rig.journal->read(1, {Range::Axis::Hlc, {0, 0}, {INT64_MAX, 0}});
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all->size(), kAppends);
    for(const auto& fetched: fetches)
    {
        ASSERT_TRUE(fetched.status.ok());
        ASSERT_EQ(fetched.trailers, 1);
        std::set<uint64_t> streamed;
        for(const auto& e: fetched.events) streamed.insert(e.id().sequence());
        for(const auto& e: *all)
        {
            v1::Hlc hlc;
            hlc.set_physical_ns(e.hlc.physical_ns);
            hlc.set_logical(e.hlc.logical);
            if(Before(hlc, fetched.trailer.sealed_frontier()))
            {
                EXPECT_TRUE(streamed.contains(e.id.sequence)) << "sequence " << e.id.sequence << " missing";
            }
        }
    }
}

} // namespace chronolog
