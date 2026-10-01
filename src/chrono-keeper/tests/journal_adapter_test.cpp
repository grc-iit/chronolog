#include <gtest/gtest.h>

#include "adapter_rig.h"

namespace chronolog
{
namespace
{

Range All() { return {Range::Axis::Hlc, {0, 0}, {INT64_MAX, 0}}; }

} // namespace

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

TEST(JournalAdapterTest, MissingEpochIsInvalidArgument)
{
    test::AdapterRig rig;
    v1::AppendRequest request;
    test::AdapterRig::fillRequest(request, 0, {1});
    v1::AppendResponse response;
    auto ctx = test::AdapterRig::context();
    auto status = rig.journal->Append(ctx.get(), request, &response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    auto events = rig.rig.journal->read(1, All());
    ASSERT_TRUE(events.ok());
    EXPECT_TRUE(events->empty());
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

TEST(JournalAdapterTest, CancelledStreamKeepsAcceptedItems)
{
    test::AdapterRig rig;
    auto ctx = test::AdapterRig::context();
    auto stream = rig.journal->AppendStream(ctx.get());
    for(uint64_t batch_id: {1, 2})
    {
        v1::AppendStreamRequest request;
        test::AdapterRig::fillRequest(request, 7, {batch_id * 2 - 1, batch_id * 2});
        request.set_batch_id(batch_id);
        ASSERT_TRUE(stream->Write(request));
        v1::AppendStreamResponse response;
        ASSERT_TRUE(stream->Read(&response));
        ASSERT_EQ(response.results(0).status().code(), 0);
    }
    ctx->TryCancel();
    EXPECT_EQ(stream->Finish().error_code(), grpc::StatusCode::CANCELLED);
    auto events = rig.rig.journal->read(1, All());
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events->size(), 4u);

    // The accepted items are still the writer's history: the next sequence continues.
    v1::AppendRequest request;
    test::AdapterRig::fillRequest(request, 7, {5});
    v1::AppendResponse response;
    auto ctx2 = test::AdapterRig::context();
    ASSERT_TRUE(rig.journal->Append(ctx2.get(), request, &response).ok());
    EXPECT_EQ(response.results(0).status().code(), 0);
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
