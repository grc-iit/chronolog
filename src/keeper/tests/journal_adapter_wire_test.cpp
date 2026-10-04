#include <gtest/gtest.h>
#include "keeper/tests/adapter_rig.h"
#include "keeper/adapter/Convert.h"
namespace chronolog
{
namespace
{
Range All() { return {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}}; }
} // namespace
TEST(JournalAdapterTest, AppendRejectionReasonsIgnoreDiagnosticText)
{
    for(uint32_t reason = 0; reason <= 13; ++reason)
    {
        AppendResult result;
        result.rejection = static_cast<AppendRejection>(reason);
        for(const auto* message: {"stale epoch", "changed unrelated diagnostic", "incarnation is released"})
        {
            result.status =
                    reason == 13 ? absl::ResourceExhaustedError(message) : absl::FailedPreconditionError(message);
            auto wire = keeper::convert::toProto(result);
            EXPECT_EQ(static_cast<uint32_t>(wire.rejection()), reason);
            EXPECT_EQ(wire.status().message(), message);
        }
    }
    test::AdapterRig rig;
    v1::AppendRequest request;
    test::AdapterRig::fillRequest(request, 7, {2, 1});
    v1::AppendResponse response;
    auto context = test::AdapterRig::context();
    ASSERT_TRUE(rig.journal->Append(context.get(), request, &response).ok());
    ASSERT_EQ(response.results_size(), 2);
    EXPECT_EQ(response.results(0).rejection(), v1::APPEND_REJECTION_SEQUENCE_GAP);
    EXPECT_EQ(response.results(1).rejection(), v1::APPEND_REJECTION_EARLIER_ITEM_FAILED);
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
TEST(JournalAdapterTest, UnspecifiedClockStatusIsUnavailable)
{
    test::AdapterRig rig;
    v1::AppendRequest request;
    test::AdapterRig::fillRequest(request, 7, {1});
    auto* reading = request.mutable_items(0)->mutable_physical();
    reading->set_physical_ns(rig.rig.clock->acceptanceClock());
    reading->set_status(v1::CLOCK_STATUS_UNSPECIFIED);
    reading->set_uncertainty_ns(1);
    auto parsed = keeper::convert::parse(request);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed->batch.items.front().physical.status, ClockStatus::Unavailable);
    EXPECT_FALSE(parsed->batch.items.front().physical.uncertainty_ns);
    EXPECT_EQ(rig.rig.clock->acceptanceClock(), 100);
    rig.rig.clock->setStatus(ClockStatus::Unavailable);
    auto context = test::AdapterRig::context();
    v1::AppendResponse response;
    ASSERT_EQ(rig.journal->Append(context.get(), request, &response).error_code(), grpc::StatusCode::OK);
    ASSERT_EQ(response.results_size(), 1);
    EXPECT_EQ(response.results(0).status().code(), 0);
    auto events = rig.rig.journal->read(1, All());
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 1u);
    EXPECT_EQ(events->front().physical.status, ClockStatus::Unavailable);
    EXPECT_FALSE(events->front().physical.uncertainty_ns);
}
} // namespace chronolog
