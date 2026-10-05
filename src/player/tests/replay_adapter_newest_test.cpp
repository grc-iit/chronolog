// Newest-first Read (I6.18), included by replay_adapter_test.cpp.

v1::ReadRequest newestRead(int64_t start, int64_t end, uint32_t max_events = 0)
{
    v1::ReadRequest request;
    request.set_story_id(kStory);
    request.mutable_hlc()->mutable_start()->set_physical_ns(start);
    request.mutable_hlc()->mutable_end()->set_physical_ns(end);
    request.set_order(v1::READ_ORDER_NEWEST_FIRST);
    request.set_max_events(max_events);
    return request;
}

void openEnd(v1::ReadRequest& request)
{
    request.mutable_hlc()->mutable_end()->set_physical_ns(std::numeric_limits<int64_t>::max());
    request.mutable_hlc()->mutable_end()->set_logical(std::numeric_limits<uint32_t>::max());
}

std::vector<int64_t> timesOf(const std::vector<v1::Event>& events)
{
    std::vector<int64_t> out;
    for(const auto& e: events) out.push_back(e.hlc().physical_ns());
    return out;
}

TEST_F(replay_adapter, NewestFirstIsHlcAxisOnly)
{
    v1::ReadRequest physical;
    physical.set_story_id(kStory);
    physical.mutable_physical()->set_start_ns(100);
    physical.mutable_physical()->set_end_ns(200);
    physical.set_order(v1::READ_ORDER_NEWEST_FIRST);
    const unsigned asked = a_.calls + b_.calls;
    auto r = read(physical);
    EXPECT_EQ(r.status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_TRUE(r.events.empty());
    EXPECT_TRUE(r.completions.empty());
    EXPECT_EQ(a_.calls + b_.calls, asked) << "no Keeper is asked for a refused request";

    // Oldest-first and unspecified orders still read the physical axis.
    for(auto order: {v1::READ_ORDER_UNSPECIFIED, v1::READ_ORDER_OLDEST_FIRST})
    {
        physical.set_order(order);
        EXPECT_TRUE(read(physical).status.ok());
    }
    physical.set_order(static_cast<v1::ReadOrder>(9));
    EXPECT_EQ(read(physical).status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    // The prefix form refuses the physical axis the same way.
    physical.clear_story_id();
    physical.set_prefix("c/a");
    physical.set_order(v1::READ_ORDER_NEWEST_FIRST);
    EXPECT_EQ(read(physical).status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    // On the HLC axis the same Read is served in descending order and names its end.
    r = read(newestRead(100, 200));
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(timesOf(r.events), (std::vector<int64_t>{160, 150, 140, 130, 120, 110}));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_TRUE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].claim_end().physical_ns(), 200);
    EXPECT_EQ(r.completions[0].frontier().physical_ns(), 200);
    EXPECT_FALSE(r.completions[0].has_claim_start());
}

TEST_F(replay_adapter, NewestFirstReadTruncatesToTheNewestAndNamesItsSuffix)
{
    auto request = newestRead(100, 0, 4);
    openEnd(request);
    auto r = read(request);
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(timesOf(r.events), (std::vector<int64_t>{160, 150, 140, 130}));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_TRUNCATED);
    EXPECT_EQ(r.completions[0].claim_start().physical_ns(), 130);
    EXPECT_EQ(r.completions[0].claim_end().physical_ns(), 200);
    EXPECT_EQ(r.completions[0].frontier().physical_ns(), 200);

    // A continuation over [start, c) finishes the history.
    r = read(newestRead(100, 130, 4));
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(timesOf(r.events), (std::vector<int64_t>{120, 110}));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_TRUE(r.completions[0].complete());

    // A source cut by its own budget covers only above the last event it returned, whatever the other holds.
    r = read(newestRead(100, 200, 1));
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(timesOf(r.events), (std::vector<int64_t>{160}));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_EQ(r.completions[0].claim_start().physical_ns(), 160);

    // A Keeper behind a given end leaves no suffix to claim.
    b_.seal(150);
    r = read(newestRead(100, 200, 4));
    ASSERT_TRUE(r.status.ok());
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_LAGGING_WRITERS);
    EXPECT_FALSE(r.completions[0].has_claim_start());
    EXPECT_EQ(r.completions[0].claim_end().physical_ns(), 200);
    EXPECT_EQ(r.completions[0].frontier().physical_ns(), 150);
    // An open end resolves to that seal.
    request = newestRead(100, 0, 4);
    openEnd(request);
    r = read(request);
    ASSERT_TRUE(r.status.ok());
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_TRUE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].claim_end().physical_ns(), 150);
    EXPECT_EQ(timesOf(r.events), (std::vector<int64_t>{140, 130, 120, 110}));
}

TEST_F(replay_adapter, NewestFirstPrefixReadMergesDescendingAndClaimsTheHighestC)
{
    seedPrefixStories(a_, b_);
    auto request = prefixRead("c/a", 100, 0);
    request.set_order(v1::READ_ORDER_NEWEST_FIRST);
    openEnd(request);
    auto r = read(request);
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(orderOf(r.events),
              (Pairs{{160, 1}, {150, 1}, {145, 3}, {140, 1}, {130, 3}, {130, 1}, {120, 1}, {110, 1}}));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_TRUE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].claim_end().physical_ns(), 200);
    EXPECT_EQ(r.completions[0].catalog_revision(), catalog_->revision());

    // The target cuts the merged stream at a whole hlc, and the claimed suffix starts at its oldest event.
    request.set_max_events(3);
    r = read(request);
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(orderOf(r.events), (Pairs{{160, 1}, {150, 1}, {145, 3}}));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_TRUNCATED);
    EXPECT_EQ(r.completions[0].claim_start().physical_ns(), 145);
    EXPECT_EQ(r.completions[0].claim_end().physical_ns(), 200);
    EXPECT_EQ(r.completions[0].catalog_revision(), catalog_->revision());

    // A story created during every attempt may hold events in the suffix, so the Read makes no claim.
    auto next = std::make_shared<std::atomic<StoryId>>(20);
    catalog_->beforeConfirm = [&, next]
    {
        const StoryId story = next->fetch_add(1);
        a_.addStory(story);
        b_.addStory(story);
        catalog_->create(story, "c/a/n" + std::to_string(story));
    };
    r = read(request);
    ASSERT_TRUE(r.status.ok());
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_LAGGING_WRITERS);
    EXPECT_FALSE(r.completions[0].has_claim_start());
}

TEST_F(replay_adapter, NewestFirstPredicateOnlyRemovesEvents)
{
    auto note = [](uint64_t writer, uint64_t sequence, int64_t hlc)
    {
        auto e = protoEvent(writer, sequence, hlc);
        e.mutable_envelope()->set_kind("note");
        return e;
    };
    a_.add(note(2, 4, 155));
    b_.add(note(4, 4, 145));
    auto request = newestRead(100, 200, 5);
    request.mutable_predicate()->add_kinds("note");
    auto r = read(request);
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(timesOf(r.events), (std::vector<int64_t>{155, 145}));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_TRUE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].claim_end().physical_ns(), 200);
    EXPECT_GT(a_.predicated.load(), 0u);
    EXPECT_GT(b_.predicated.load(), 0u);

    // The target counts matches, so one note is a cut at 155 and the claim starts there.
    request.set_max_events(1);
    r = read(request);
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(timesOf(r.events), (std::vector<int64_t>{155}));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_TRUNCATED);
    EXPECT_EQ(r.completions[0].claim_start().physical_ns(), 155);
}
