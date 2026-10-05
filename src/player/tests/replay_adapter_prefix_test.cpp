// Prefix Read and Tail (I6.15, I6.16, I7.7), included by replay_adapter_test.cpp.

using Pairs = std::vector<std::pair<int64_t, uint64_t>>;

v1::ReadRequest prefixRead(const std::string& prefix, int64_t start, int64_t end)
{
    v1::ReadRequest request;
    request.set_prefix(prefix);
    request.mutable_hlc()->mutable_start()->set_physical_ns(start);
    request.mutable_hlc()->mutable_end()->set_physical_ns(end);
    return request;
}

// (hlc, story) of every event in stream order.
Pairs orderOf(const std::vector<v1::Event>& events)
{
    Pairs out;
    for(const auto& e: events) out.emplace_back(e.hlc().physical_ns(), e.id().story_id());
    return out;
}

// Story 3 lies under "c/a" and shares an hlc and a writer with story 1 at 130; story 5 is under "c/ab" and never matches.
void seedPrefixStories(FakeArchive& a, FakeArchive& b)
{
    for(FakeArchive* keeper: {&a, &b})
        for(StoryId story: {3, 5}) keeper->addStory(story);
    a.add(storyEvent(3, 2, 1, 130));
    b.add(storyEvent(3, 4, 1, 145));
    a.add(storyEvent(5, 2, 1, 135));
}

const Pairs kPrefixOrder{{110, 1}, {120, 1}, {130, 1}, {130, 3}, {140, 1}, {145, 3}, {150, 1}, {160, 1}};

TEST_F(replay_adapter, PrefixReadIsCompleteOnlyAtOneCatalogRevision)
{
    seedPrefixStories(a_, b_);
    auto r = read(prefixRead("c/a", 100, 200));
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(orderOf(r.events), kPrefixOrder);
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_TRUE(r.completions[0].complete());
    ASSERT_TRUE(r.completions[0].has_catalog_revision());
    EXPECT_EQ(r.completions[0].catalog_revision(), catalog_->revision());

    // A Keeper behind the end leaves the Read incomplete although the story set is one revision.
    b_.seal(150);
    r = read(prefixRead("c/a", 100, 200));
    ASSERT_TRUE(r.status.ok());
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_FALSE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_LAGGING_WRITERS);
    EXPECT_EQ(r.completions[0].catalog_revision(), catalog_->revision());
    EXPECT_EQ(r.completions[0].frontier().physical_ns(), 150);
}

TEST_F(replay_adapter, PrefixReadWithMaxEventsTruncatesAtAWholeHlcAndHoldsNothingAtOrAboveC)
{
    seedPrefixStories(a_, b_);
    auto request = prefixRead("c/a", 100, 200);
    request.set_max_events(4);
    auto r = read(request);
    ASSERT_TRUE(r.status.ok());
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_TRUNCATED);
    const auto c = r.completions[0].frontier().physical_ns();
    EXPECT_EQ(c, 140);
    EXPECT_EQ(orderOf(r.events), (Pairs{{110, 1}, {120, 1}, {130, 1}, {130, 3}}));
}

TEST_F(replay_adapter, PrefixOverTheStoryCapIsResourceExhausted)
{
    seedPrefixStories(a_, b_);
    for(StoryId story = 1000; story <= 1000 + PrefixOptions{}.max_scopes; ++story)
        catalog_->create(story, "big/s" + std::to_string(story));
    auto r = read(prefixRead("big", 100, 200));
    EXPECT_EQ(r.status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
    EXPECT_TRUE(r.events.empty());
    EXPECT_TRUE(r.completions.empty());

    v1::TailRequest tail;
    tail.set_prefix("big");
    tail.set_progress(true);
    tail.mutable_from()->mutable_hlc()->set_physical_ns(100);
    auto ctx = context();
    auto reader = stub_->Tail(ctx.get(), tail);
    v1::TailResponse response;
    EXPECT_FALSE(reader->Read(&response));
    EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);

    // A prefix under the cap is served.
    r = read(prefixRead("c/a/x", 100, 200));
    EXPECT_TRUE(r.status.ok());
    EXPECT_EQ(orderOf(r.events), (Pairs{{130, 3}, {145, 3}}));
}

TEST_F(replay_adapter, PrefixTailFixesItsStorySetAtR)
{
    seedPrefixStories(a_, b_);
    v1::TailRequest request;
    request.set_prefix("c/a");
    request.set_progress(true);
    request.mutable_from()->mutable_hlc()->set_physical_ns(100);
    auto ctx = context();
    auto reader = stub_->Tail(ctx.get(), request);
    const uint64_t revision = catalog_->revision();
    v1::TailResponse response;
    ASSERT_TRUE(reader->Read(&response));
    ASSERT_TRUE(response.has_progress());
    EXPECT_EQ(response.progress().catalog_revision(), revision);
    EXPECT_EQ(response.progress().position().hlc().physical_ns(), 100);
    std::vector<v1::Event> seen;
    auto pump = [&](size_t want)
    {
        for(int i = 0; i < 100 && seen.size() < want && reader->Read(&response); ++i)
        {
            EXPECT_FALSE(response.has_completion());
            for(const auto& e: response.batch().events()) seen.push_back(e);
        }
    };
    pump(kPrefixOrder.size());
    EXPECT_EQ(orderOf(seen), kPrefixOrder);

    // A story created after R is not in the set, so an event of it below a later seal is never delivered.
    catalog_->create(9, "c/a/z");
    a_.addStory(9);
    a_.add(storyEvent(9, 2, 1, 240));
    a_.add(protoEvent(2, 4, 250));
    a_.seal(300);
    b_.seal(300);
    pump(kPrefixOrder.size() + 1);
    ASSERT_EQ(seen.size(), kPrefixOrder.size() + 1);
    EXPECT_EQ(orderOf({seen.back()}), (Pairs{{250, 1}}));
    for(const auto& e: seen) EXPECT_NE(e.id().story_id(), 9u);
    ctx->TryCancel();
    while(reader->Read(&response)) {}
    EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::CANCELLED);
}

TEST_F(replay_adapter, PrefixTailRequiresProgressAndExactlyOneName)
{
    v1::TailRequest request;
    request.set_prefix("c/a");
    request.mutable_from()->mutable_hlc()->set_physical_ns(100);
    for(int variant = 0; variant < 3; ++variant)
    {
        if(variant == 1)
        {
            request.set_progress(true);
            request.set_story_id(kStory);
        }
        if(variant == 2)
        {
            request.clear_prefix();
            request.clear_story_id();
        }
        auto ctx = context();
        auto reader = stub_->Tail(ctx.get(), request);
        v1::TailResponse response;
        EXPECT_FALSE(reader->Read(&response));
        EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::INVALID_ARGUMENT) << variant;
    }
    auto both = prefixRead("c/a", 100, 200);
    both.set_story_id(kStory);
    EXPECT_EQ(read(both).status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    auto neither = hlcRead(100, 200);
    neither.clear_story_id();
    EXPECT_EQ(read(neither).status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(replay_adapter, PrefixReadExcludesAStoryTombstonedAtR)
{
    seedPrefixStories(a_, b_);
    catalog_->tombstone(3);
    auto r = read(prefixRead("c/a", 100, 200));
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(orderOf(r.events), (Pairs{{110, 1}, {120, 1}, {130, 1}, {140, 1}, {150, 1}, {160, 1}}));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_TRUE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].catalog_revision(), catalog_->revision());

    // A story of the set destroyed while the Read runs ends it SOURCE_FAILED and leaves none of its events.
    for(FakeArchive* keeper: {&a_, &b_}) keeper->addStory(7);
    a_.add(storyEvent(7, 2, 1, 135));
    catalog_->create(7, "c/a/y");
    catalog_->beforeConfirm = [&] { catalog_->tombstone(7); };
    r = read(prefixRead("c/a", 100, 200));
    ASSERT_TRUE(r.status.ok());
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_FALSE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_SOURCE_FAILED);
    for(const auto& e: r.events) EXPECT_EQ(e.id().story_id(), 1u);
}

TEST_F(replay_adapter, EmptyPrefixMakesNoClaim)
{
    const auto r = read(prefixRead("c/none", 100, 200));
    ASSERT_TRUE(r.status.ok());
    EXPECT_TRUE(r.events.empty());
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_FALSE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_LAGGING_WRITERS);
    ASSERT_TRUE(r.completions[0].has_catalog_revision());
    EXPECT_EQ(r.completions[0].catalog_revision(), catalog_->revision());
}

TEST_F(replay_adapter, PrefixReadReResolvesWhenAStoryIsCreatedDuringIt)
{
    seedPrefixStories(a_, b_);
    for(FakeArchive* keeper: {&a_, &b_}) keeper->addStory(7);
    a_.add(storyEvent(7, 2, 1, 155));
    auto created = std::make_shared<std::atomic<bool>>(false);
    catalog_->beforeConfirm = [&, created]
    {
        if(!created->exchange(true))
            catalog_->create(7, "c/a/y");
    };
    auto r = read(prefixRead("c/a", 100, 200));
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(catalog_->confirmed(), 2u);
    EXPECT_EQ(orderOf(r.events),
              (Pairs{{110, 1}, {120, 1}, {130, 1}, {130, 3}, {140, 1}, {145, 3}, {150, 1}, {155, 7}, {160, 1}}));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_TRUE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].catalog_revision(), catalog_->revision());

    // A story created during every attempt ends the Read LAGGING_WRITERS after prefix_resolve_retries re-resolutions.
    auto next = std::make_shared<std::atomic<StoryId>>(20);
    catalog_->beforeConfirm = [&, next]
    {
        const StoryId story = next->fetch_add(1);
        a_.addStory(story);
        b_.addStory(story);
        catalog_->create(story, "c/a/n" + std::to_string(story));
    };
    const unsigned before = catalog_->confirmed();
    r = read(prefixRead("c/a", 100, 200));
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(catalog_->confirmed() - before, 4u);
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_FALSE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_LAGGING_WRITERS);
}
