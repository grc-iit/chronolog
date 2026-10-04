#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>

#include "keeper/tests/adapter_rig.h"

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

TEST(ArchiveTransferTest, WriterStatusReturnsRetainedCheckpointAndFencesIdentity)
{
    test::AdapterRig rig;
    auto appended = rig.rig.journal->append({1, 7, {Item(1)}}, Durability::Accepted);
    ASSERT_TRUE(appended.ok());
    rig.rig.journal->releaseWriter(1, 2, 3, AcquisitionTerminationCause::Expired);
    rig.rig.journal->eraseEvents(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    iv1::WriterStatusRequest request;
    request.set_story_id(1);
    request.set_writer_id(2);
    request.set_incarnation(3);
    request.set_sequence(1);
    request.set_expect_epoch(7);
    iv1::WriterStatusResponse response;
    auto ctx = test::AdapterRig::context();
    ASSERT_TRUE(rig.archive->WriterStatus(ctx.get(), request, &response).ok());
    EXPECT_TRUE(response.known());
    EXPECT_TRUE(response.released());
    EXPECT_EQ(response.next_sequence(), 2u);
    EXPECT_EQ(response.termination_cause(), v1::ACQUISITION_TERMINATION_CAUSE_EXPIRED);
    EXPECT_EQ(response.recorded_hlc().physical_ns(), appended->at(0).hlc.physical_ns);
    EXPECT_TRUE(Before(response.recorded_hlc(), response.sealed_frontier()));
    request.set_expect_instance("wrong");
    ctx = test::AdapterRig::context();
    EXPECT_EQ(rig.archive->WriterStatus(ctx.get(), request, &response).error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
    request.clear_expect_instance();
    request.set_expect_epoch(8);
    ctx = test::AdapterRig::context();
    EXPECT_EQ(rig.archive->WriterStatus(ctx.get(), request, &response).error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
}

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

TEST(ArchiveTransferTest, FetchHotBoundsEncodedAttributesAndAdmitsOversizedFirstEvent)
{
    test::AdapterRig rig;
    std::vector<AppendItem> items;
    for(uint64_t sequence = 1; sequence <= 4; ++sequence)
    {
        auto item = Item(sequence);
        item.envelope.attributes["padding"] = std::string(sequence == 3 ? 5 << 20 : 1 << 20, 'a');
        items.push_back(std::move(item));
    }
    auto appended = rig.rig.journal->append({1, 7, items}, Durability::Accepted);
    ASSERT_TRUE(appended.ok());
    ASSERT_EQ(appended->size(), items.size());
    for(const auto& result: *appended) ASSERT_TRUE(result.status.ok());
    auto ctx = test::AdapterRig::context();
    auto reader = rig.archive->FetchHot(ctx.get(), AllRequest());
    iv1::FetchHotResponse response;
    size_t delivered = 0;
    size_t batches = 0;
    bool complete = false;
    for(size_t messages = 0; messages < 10 && reader->Read(&response); ++messages)
    {
        size_t bytes = 0;
        for(const auto& event: response.batch().events())
        {
            bytes += event.ByteSizeLong();
            ASSERT_LT(delivered, items.size());
            EXPECT_EQ(event.id().sequence(), items[delivered].sequence);
            EXPECT_EQ(event.envelope().attributes().at("padding"), items[delivered].envelope.attributes.at("padding"));
            ++delivered;
        }
        EXPECT_TRUE(bytes <= kEventBatchBytes || response.batch().events_size() == 1);
        if(response.has_batch())
            ++batches;
        if(response.has_trailer())
            complete = !response.trailer().truncated();
    }
    reader->Finish();
    EXPECT_EQ(delivered, 4u);
    EXPECT_EQ(batches, 4u);
    EXPECT_TRUE(complete);
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
} // namespace chronolog

namespace chronolog
{
TEST(ArchiveTransferTest, PhysicalFilterCountsOnlyIntervalMatches)
{
    test::AdapterRig rig;
    auto first = Item(1), second = Item(2), third = Item(3);
    first.physical = {10, 1, ClockStatus::Synced};
    second.physical = {100, 10, ClockStatus::Synced};
    third.physical = {200, 1, ClockStatus::Synced};
    auto appended = rig.rig.journal->append({1, 7, {first, second, third}}, Durability::Accepted);
    ASSERT_TRUE(appended.ok());
    for(const auto& result: *appended) ASSERT_TRUE(result.status.ok());
    auto request = AllRequest(1);
    request.mutable_physical_filter()->set_start_ns(105);
    request.mutable_physical_filter()->set_end_ns(110);
    auto fetched = FetchAll(*rig.archive, request);
    ASSERT_TRUE(fetched.status.ok());
    ASSERT_EQ(fetched.events.size(), 1u);
    EXPECT_EQ(fetched.events.front().id().sequence(), 2u);
    EXPECT_FALSE(fetched.trailer.truncated());
    EXPECT_TRUE(fetched.trailer.has_physical_frontier_ns());
    request.mutable_physical_filter()->set_start_ns(0);
    request.mutable_physical_filter()->set_end_ns(250);
    fetched = FetchAll(*rig.archive, request);
    EXPECT_EQ(fetched.events.size(), 1u);
    EXPECT_TRUE(fetched.trailer.truncated());
}
} // namespace chronolog
