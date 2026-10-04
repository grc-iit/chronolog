#include <gtest/gtest.h>

#include "adapter_rig.h"
#include "membership/AcquisitionWatcher.h"

namespace chronolog
{
namespace
{

Range All() { return {Range::Axis::Hlc, {0, 0}, {INT64_MAX, 0}}; }

} // namespace

TEST(JournalAdapterTest, TerminationCausesAreTyped)
{
    for(auto cause: {v1::ACQUISITION_TERMINATION_CAUSE_EXPIRED,
                     v1::ACQUISITION_TERMINATION_CAUSE_OWNER_REMOVED,
                     v1::ACQUISITION_TERMINATION_CAUSE_RELEASED,
                     v1::ACQUISITION_TERMINATION_CAUSE_SUPERSEDED,
                     v1::ACQUISITION_TERMINATION_CAUSE_UNSPECIFIED})
    {
        test::AdapterRig rig;
        keeper::AcquisitionWatcher watcher(*rig.rig.journal, "self", nullptr, false);
        internal::v1::AcquisitionUpdate update;
        update.set_story_id(1);
        update.set_writer_id(2);
        update.set_incarnation(3);
        update.set_revision(1);
        update.set_state(internal::v1::ACQUISITION_STATE_RELEASED);
        update.set_termination_cause(cause);
        watcher.applyUpdate(update);
        v1::AppendRequest request;
        test::AdapterRig::fillRequest(request, 7, {1, 2});
        v1::AppendResponse response;
        auto ctx = test::AdapterRig::context();
        ASSERT_TRUE(rig.journal->Append(ctx.get(), request, &response).ok());
        ASSERT_EQ(response.results_size(), 2);
        auto expected = v1::APPEND_REJECTION_FENCED_RELEASED;
        if(cause == v1::ACQUISITION_TERMINATION_CAUSE_EXPIRED)
            expected = v1::APPEND_REJECTION_FENCED_EXPIRED;
        if(cause == v1::ACQUISITION_TERMINATION_CAUSE_OWNER_REMOVED)
            expected = v1::APPEND_REJECTION_FENCED_OWNER_REMOVED;
        if(cause == v1::ACQUISITION_TERMINATION_CAUSE_SUPERSEDED)
            expected = v1::APPEND_REJECTION_FENCED_SUPERSEDED;
        for(const auto& result: response.results())
        {
            EXPECT_EQ(result.status().code(), 9);
            EXPECT_EQ(result.rejection(), expected);
        }
    }
}

TEST(JournalAdapterTest, RegistrationLagAndSequenceGapHaveNoTerminationCause)
{
    test::AdapterRig rig;
    v1::AppendRequest request;
    test::AdapterRig::fillRequest(request, 7, {2});
    *request.add_items() = test::AdapterRig::item(1, 99);
    v1::AppendResponse response;
    auto ctx = test::AdapterRig::context();
    ASSERT_TRUE(rig.journal->Append(ctx.get(), request, &response).ok());
    ASSERT_EQ(response.results_size(), 2);
    EXPECT_EQ(response.results(0).rejection(), v1::APPEND_REJECTION_SEQUENCE_GAP);
    EXPECT_EQ(response.results(1).rejection(), v1::APPEND_REJECTION_NOT_REGISTERED);
}

TEST(JournalAdapterTest, AppendKeepsRequestOrderAndIsIdempotent)
{
    test::AdapterRig rig;
    v1::AppendRequest request;
    test::AdapterRig::fillRequest(request, 7, {1, 2, 3});
    request.set_batch_id(42);
    v1::AppendResponse first;
    auto ctx = test::AdapterRig::context();
    ASSERT_TRUE(rig.journal->Append(ctx.get(), request, &first).ok());
    EXPECT_EQ(first.batch_id(), 42u);
    ASSERT_EQ(first.results_size(), 3);
    for(int i = 0; i < 3; ++i)
    {
        EXPECT_EQ(first.results(i).status().code(), 0);
        EXPECT_EQ(first.results(i).id().sequence(), static_cast<uint64_t>(i + 1));
        EXPECT_EQ(first.results(i).achieved_durability(), v1::DURABILITY_ACCEPTED);
        EXPECT_GT(first.results(i).assigned_hlc().physical_ns(), 0);
    }
    EXPECT_FALSE(first.has_current_route());

    v1::AppendResponse retry;
    auto ctx2 = test::AdapterRig::context();
    ASSERT_TRUE(rig.journal->Append(ctx2.get(), request, &retry).ok());
    for(int i = 0; i < 3; ++i)
    {
        EXPECT_EQ(retry.results(i).status().code(), 0);
        EXPECT_EQ(retry.results(i).assigned_hlc().physical_ns(), first.results(i).assigned_hlc().physical_ns());
        EXPECT_EQ(retry.results(i).assigned_hlc().logical(), first.results(i).assigned_hlc().logical());
    }
    auto events = rig.rig.journal->read(1, All());
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events->size(), 3u);
}

TEST(JournalAdapterTest, GapAndDurableAreItemFailuresWithGrpcOk)
{
    test::AdapterRig rig;
    v1::AppendRequest request;
    test::AdapterRig::fillRequest(request, 7, {1, 3});
    v1::AppendResponse response;
    auto ctx = test::AdapterRig::context();
    ASSERT_TRUE(rig.journal->Append(ctx.get(), request, &response).ok());
    EXPECT_EQ(response.results(0).status().code(), 0);
    EXPECT_EQ(response.results(1).status().code(), 9);
    EXPECT_NE(response.results(1).status().message().find("expected sequence 2"), std::string::npos);

    request.set_durability(v1::DURABILITY_DURABLE);
    v1::AppendResponse durable;
    auto ctx2 = test::AdapterRig::context();
    ASSERT_TRUE(rig.journal->Append(ctx2.get(), request, &durable).ok());
    for(const auto& result: durable.results())
    {
        EXPECT_EQ(result.status().code(), 12);
        EXPECT_EQ(result.achieved_durability(), v1::DURABILITY_UNSPECIFIED);
    }
}

TEST(JournalAdapterTest, StaleEpochIsGrpcOkWithRouteAtEveryLevel)
{
    test::AdapterRig rig;
    v1::AppendRequest request;
    test::AdapterRig::fillRequest(request, 6, {1, 2});
    v1::AppendResponse response;
    auto ctx = test::AdapterRig::context();
    ASSERT_TRUE(rig.journal->Append(ctx.get(), request, &response).ok());
    ASSERT_EQ(response.results_size(), 2);
    for(const auto& result: response.results())
    {
        EXPECT_EQ(result.status().code(), 9);
        EXPECT_EQ(result.current_route().epoch(), 7u);
    }
    ASSERT_TRUE(response.has_current_route());
    EXPECT_EQ(response.current_route().epoch(), 7u);
    EXPECT_EQ(response.current_route().keepers(0).process_id(), "self");
}

TEST(JournalAdapterTest, AppendStreamEchoesBatchIdsInOrder)
{
    test::AdapterRig rig;
    auto ctx = test::AdapterRig::context();
    auto stream = rig.journal->AppendStream(ctx.get());
    uint64_t next_sequence = 1;
    for(uint64_t batch_id: {11, 12, 13})
    {
        v1::AppendStreamRequest request;
        test::AdapterRig::fillRequest(request, 7, {next_sequence, next_sequence + 1});
        next_sequence += 2;
        request.set_batch_id(batch_id);
        ASSERT_TRUE(stream->Write(request));
        v1::AppendStreamResponse response;
        ASSERT_TRUE(stream->Read(&response));
        EXPECT_EQ(response.batch_id(), batch_id);
        ASSERT_EQ(response.results_size(), 2);
        EXPECT_EQ(response.results(0).status().code(), 0);
        EXPECT_EQ(response.results(1).status().code(), 0);
    }
    stream->WritesDone();
    EXPECT_TRUE(stream->Finish().ok());
    auto events = rig.rig.journal->read(1, All());
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events->size(), 6u);
}

TEST(JournalAdapterTest, MalformedStreamRequestEndsWithInvalidArgument)
{
    test::AdapterRig rig;
    auto ctx = test::AdapterRig::context();
    auto stream = rig.journal->AppendStream(ctx.get());
    v1::AppendStreamRequest request;
    test::AdapterRig::fillRequest(request, 0, {1});
    ASSERT_TRUE(stream->Write(request));
    v1::AppendStreamResponse response;
    EXPECT_FALSE(stream->Read(&response));
    EXPECT_EQ(stream->Finish().error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

} // namespace chronolog
