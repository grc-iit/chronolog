using ReplayAdapterTest = replay_adapter;

TEST_F(ReplayAdapterTest, UnsetRangeIsInvalidArgument)
{
    v1::ReadRequest request;
    request.set_story_id(kStory);
    auto r = read(request);
    EXPECT_EQ(r.status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_TRUE(r.events.empty());
}
