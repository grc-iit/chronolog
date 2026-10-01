// Reusable suite: include this .cpp in the implementation instantiation TU,
// provide the documented fresh harness factory, then INSTANTIATE_TEST_SUITE_P.
// Do not also compile that suite separately into the same test executable.
#include <gtest/gtest.h>
#include "chronolog/replay.h"
namespace chronolog::contract
{
// Writer 4 is on a second Keeper; writer 2 and any added idle writer share one Keeper.
// Factory seeds story 1 with two writers (2,3)/(4,3), overlap duplicates,
// and an unconfirmed event at HLC {150,0} below the archive boundary 200.
// Every event lies in [100,300). setFrontiers controls each registered writer;
// failSource/truncateSource simulate Keeper failures; finishTail terminates tail.
struct ReplayHarness
{
    std::unique_ptr<Replay> sut;
    // Route contains keeper-a and keeper-b. Diagnostic acquisition view can
    // omit a writer; its Keeper seal still determines completeness.
    std::function<void(std::vector<KeeperFrontier>)> setKeeperFrontiers;
    std::function<void()> hideWriterFromAcquisitionView;
    std::function<void(std::vector<Frontier>)> setFrontiers;
    std::function<void()> failSource;
    std::function<void()> truncateSource;
    std::function<void()> finishTail;
    // registerIdleWriter adds an incarnation on the same Keeper as writer 2.
    std::function<void(uint64_t, uint64_t)> registerIdleWriter;
    // Seeded events for this test are DURABLE; crashRestart reopens persisted state.
    std::function<void()> crashRestart;
    // Tombstone story 1; lose an archived window below a preserved watermark.
    std::function<void()> tombstoneStory, loseWindowBelowWatermark;
};
Range Query() { return {Range::Axis::Hlc, {100, 0}, {300, 0}}; }
struct Collected
{
    std::vector<Event> events;
    std::vector<Completion> completions;
    bool batch_after_completion{};
};
absl::StatusOr<Collected> Collect(ReplayStream& stream)
{
    Collected c;
    for(size_t n = 0; n < 10000; ++n)
    {
        auto b = stream.next();
        if(!b.ok())
            return b.status();
        if(!*b)
            return c;
        if(!c.completions.empty())
            c.batch_after_completion = true;
        for(auto& e: (**b).events) c.events.push_back(e);
        if((**b).completion)
            c.completions.push_back(*(**b).completion);
    }
    return absl::ResourceExhaustedError("stream did not terminate");
}

using ReplayFactory = std::function<std::unique_ptr<ReplayHarness>()>;
class ReplayContract: public ::testing::TestWithParam<ReplayFactory>
{
protected:
    std::unique_ptr<ReplayHarness> h;
    void SetUp() override
    {
        h = GetParam()();
        ASSERT_NE(h, nullptr);
        ASSERT_NE(h->sut, nullptr);
    }
};

TEST_P(ReplayContract, ReadEndsWithExactlyOneCompletion)
{
    auto s = h->sut->read(1, Query());
    ASSERT_TRUE(s.ok());
    auto c = Collect(**s);
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c->completions.size(), 1u);
    EXPECT_FALSE(c->batch_after_completion);
}

TEST_P(ReplayContract, CompleteWhenEveryKeeperSealReachesEnd)
{
    ASSERT_TRUE(h->setFrontiers);
    h->setFrontiers({{2, 3, {301, 0}}, {4, 3, {301, 0}}});
    auto s = h->sut->read(1, Query());
    ASSERT_TRUE(s.ok());
    auto c = Collect(**s);
    ASSERT_TRUE(c.ok());
    ASSERT_EQ(c->completions.size(), 1u);
    EXPECT_TRUE(c->completions[0].complete);
    EXPECT_TRUE(c->completions[0].laggards.empty());
}

TEST_P(ReplayContract, CompletionNamesLaggardsAndEqualityIsSufficient)
{
    ASSERT_TRUE(h->setFrontiers);
    h->setFrontiers({{2, 3, {301, 0}}, {4, 3, {300, 0}}});
    auto stream = h->sut->read(1, Query());
    ASSERT_TRUE(stream.ok());
    auto result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->completions.size(), 1u);
    EXPECT_TRUE(result->completions[0].complete);
    EXPECT_TRUE(result->completions[0].laggards.empty());
    EXPECT_EQ(result->completions[0].reason, IncompleteReason::None);
    h->setFrontiers({{2, 3, {301, 0}}, {4, 3, {299, 0}}});
    stream = h->sut->read(1, Query());
    ASSERT_TRUE(stream.ok());
    result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->completions.size(), 1u);
    EXPECT_FALSE(result->completions[0].complete);
    EXPECT_EQ(result->completions[0].reason, IncompleteReason::LaggingWriters);
    ASSERT_EQ(result->completions[0].laggards.size(), 1u);
    EXPECT_EQ(result->completions[0].laggards[0].writer_id, 4u);
    EXPECT_EQ(result->completions[0].laggards[0].frontier, (Hlc{299, 0}));
}

TEST_P(ReplayContract, FailedSourcePreventsCompleteness)
{
    ASSERT_TRUE(h->failSource);
    h->failSource();
    auto s = h->sut->read(1, Query());
    ASSERT_TRUE(s.ok());
    auto c = Collect(**s);
    ASSERT_TRUE(c.ok());
    ASSERT_EQ(c->completions.size(), 1u);
    EXPECT_FALSE(c->completions[0].complete);
    EXPECT_EQ(c->completions[0].reason, IncompleteReason::SourceFailed);
}

TEST_P(ReplayContract, TruncatedSourcePreventsCompleteness)
{
    ASSERT_TRUE(h->truncateSource);
    h->truncateSource();
    auto s = h->sut->read(1, Query());
    ASSERT_TRUE(s.ok());
    auto c = Collect(**s);
    ASSERT_TRUE(c.ok());
    ASSERT_EQ(c->completions.size(), 1u);
    EXPECT_FALSE(c->completions[0].complete);
    EXPECT_EQ(c->completions[0].reason, IncompleteReason::Truncated);
}

TEST_P(ReplayContract, TotalOrderAndEventIdentityDeduplication)
{
    auto s = h->sut->read(1, Query());
    ASSERT_TRUE(s.ok());
    auto c = Collect(**s);
    ASSERT_TRUE(c.ok());
    ASSERT_GE(c->events.size(), 2u);
    for(size_t i = 1; i < c->events.size(); ++i) EXPECT_TRUE(ReplayLess(c->events[i - 1], c->events[i]));
    for(size_t i = 0; i < c->events.size(); ++i)
        for(size_t j = i + 1; j < c->events.size(); ++j) EXPECT_NE(c->events[i].id, c->events[j].id);
}

TEST_P(ReplayContract, UnconfirmedHotEventsBelowBoundarySurvive)
{
    auto s = h->sut->read(1, Query());
    ASSERT_TRUE(s.ok());
    auto c = Collect(**s);
    ASSERT_TRUE(c.ok());
    bool found = false;
    for(const auto& e: c->events) found |= e.hlc == Hlc{150, 0};
    EXPECT_TRUE(found);
}

TEST_P(ReplayContract, HalfOpenRange)
{
    auto s = h->sut->read(1, Query());
    ASSERT_TRUE(s.ok());
    auto c = Collect(**s);
    ASSERT_TRUE(c.ok());
    for(const auto& e: c->events)
    {
        EXPECT_GE(e.hlc, Query().start);
        EXPECT_LT(e.hlc, Query().end);
    }
}

TEST_P(ReplayContract, TailNeverClaimsCompleteness)
{
    ASSERT_TRUE(h->finishTail);
    Event p;
    p.id.story_id = 1;
    auto s = h->sut->tail(1, p);
    ASSERT_TRUE(s.ok());
    h->finishTail();
    auto c = Collect(**s);
    ASSERT_TRUE(c.ok());
    ASSERT_EQ(c->completions.size(), 1u);
    EXPECT_FALSE(c->completions[0].complete);
}

TEST_P(ReplayContract, CancellationIsIdempotent)
{
    Event p;
    p.id.story_id = 1;
    auto s = h->sut->tail(1, p);
    ASSERT_TRUE(s.ok());
    (*s)->cancel();
    (*s)->cancel();
    auto next = (*s)->next();
    EXPECT_TRUE(!next.ok() || !*next || ((**next).completion && !(**next).completion->complete));
}

TEST_P(ReplayContract, PhysicalAxisAlwaysIncomplete)
{
    ASSERT_TRUE(h->setFrontiers);
    h->setFrontiers({{2, 3, {301, 0}}, {4, 3, {301, 0}}});
    auto range = Query();
    range.axis = Range::Axis::Physical;
    auto stream = h->sut->read(1, range);
    ASSERT_TRUE(stream.ok());
    auto result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->completions.size(), 1u);
    EXPECT_FALSE(result->completions[0].complete);
    EXPECT_EQ(result->completions[0].reason, IncompleteReason::PhysicalAxisUnbounded);
}

TEST_P(ReplayContract, TailResumesExclusivelyAfterPosition)
{
    ASSERT_TRUE(h->finishTail);
    auto read = h->sut->read(1, Query());
    ASSERT_TRUE(read.ok());
    auto events = Collect(**read);
    ASSERT_TRUE(events.ok());
    ASSERT_FALSE(events->events.empty());
    auto position = events->events.front();
    auto tail = h->sut->tail(1, position);
    ASSERT_TRUE(tail.ok());
    h->finishTail();
    auto resumed = Collect(**tail);
    ASSERT_TRUE(resumed.ok());
    for(const auto& event: resumed->events) EXPECT_TRUE(ReplayLess(position, event));
}

TEST_P(ReplayContract, IdleRegisteredWriterDoesNotBlockCompleteness)
{
    ASSERT_TRUE(h->registerIdleWriter);
    ASSERT_TRUE(h->setFrontiers);
    h->registerIdleWriter(6, 3);
    h->setFrontiers({{2, 3, {300, 0}}, {4, 3, {300, 0}}, {6, 3, {300, 0}}});
    auto stream = h->sut->read(1, Query());
    ASSERT_TRUE(stream.ok());
    auto result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->completions.size(), 1u);
    EXPECT_TRUE(result->completions[0].complete);
    EXPECT_TRUE(result->completions[0].laggards.empty());
}
TEST_P(ReplayContract, CompleteReadIsStableForDurableEvents)
{
    ASSERT_TRUE(h->crashRestart);
    ASSERT_TRUE(h->setFrontiers);
    h->setFrontiers({{2, 3, {300, 0}}, {4, 3, {300, 0}}});
    auto stream = h->sut->read(1, Query());
    ASSERT_TRUE(stream.ok());
    auto before = Collect(**stream);
    ASSERT_TRUE(before.ok());
    ASSERT_EQ(before->completions.size(), 1u);
    ASSERT_TRUE(before->completions[0].complete);
    h->crashRestart();
    stream = h->sut->read(1, Query());
    ASSERT_TRUE(stream.ok());
    auto after = Collect(**stream);
    ASSERT_TRUE(after.ok());
    ASSERT_EQ(before->events.size(), after->events.size());
    for(size_t i = 0; i < before->events.size(); ++i)
    {
        EXPECT_EQ(before->events[i].id, after->events[i].id);
        EXPECT_EQ(before->events[i].hlc, after->events[i].hlc);
        EXPECT_EQ(before->events[i].envelope.payload, after->events[i].envelope.payload);
    }
}
TEST_P(ReplayContract, UnknownWriterCoveredByItsKeeperSeal)
{
    ASSERT_TRUE(h->setKeeperFrontiers);
    ASSERT_TRUE(h->hideWriterFromAcquisitionView);
    h->hideWriterFromAcquisitionView();
    h->setKeeperFrontiers({{"keeper-a", 7, {300, 0}, true}, {"keeper-b", 7, {299, 0}, true}});
    auto stream = h->sut->read(1, Query());
    ASSERT_TRUE(stream.ok());
    auto result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->completions.size(), 1u);
    EXPECT_FALSE(result->completions[0].complete);
    h->setKeeperFrontiers({{"keeper-a", 7, {300, 0}, true}, {"keeper-b", 7, {300, 0}, true}});
    stream = h->sut->read(1, Query());
    ASSERT_TRUE(stream.ok());
    result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->completions.size(), 1u);
    EXPECT_TRUE(result->completions[0].complete);
    h->setKeeperFrontiers({{"keeper-a", 7, {300, 0}, true}, {"keeper-b", 7, {300, 0}, false}});
    stream = h->sut->read(1, Query());
    ASSERT_TRUE(stream.ok());
    result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->completions.size(), 1u);
    EXPECT_FALSE(result->completions[0].complete);
    EXPECT_EQ(result->completions[0].reason, IncompleteReason::SourceFailed);
}
TEST_P(ReplayContract, ReadOnTombstonedStoryFails)
{
    ASSERT_TRUE(h->tombstoneStory);
    h->tombstoneStory();
    auto stream = h->sut->read(1, Query());
    EXPECT_EQ(stream.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST_P(ReplayContract, LostWindowBelowWatermarkIsSourceFailed)
{
    ASSERT_TRUE(h->loseWindowBelowWatermark);
    ASSERT_TRUE(h->setFrontiers);
    h->setFrontiers({{2, 3, {300, 0}}, {4, 3, {300, 0}}});
    h->loseWindowBelowWatermark();
    auto stream = h->sut->read(1, Query());
    ASSERT_TRUE(stream.ok());
    auto result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->completions.size(), 1u);
    EXPECT_FALSE(result->completions[0].complete);
    EXPECT_EQ(result->completions[0].reason, IncompleteReason::SourceFailed);
}
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(ReplayContract);
} // namespace chronolog::contract
