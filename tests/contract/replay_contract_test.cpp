// Reusable suite: include this .cpp in the implementation instantiation TU,
// provide the documented fresh harness factory, then INSTANTIATE_TEST_SUITE_P.
// Do not also compile that suite separately into the same test executable.
#include <gtest/gtest.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "chronolog/replay.h"
namespace chronolog::contract
{
// What one Keeper of story 1 holds and reports to a Tail: the events its scans can see, its sealed frontier, whether
// its answer is cut short whatever the request, for a Keeper that left the Route the cut it left at, its instance, and
// the number of events one answer holds before it is cut (zero is no limit). A Keeper answers from the lower bound the
// request names and never returns an event at or above its seal that it has not made visible.
struct KeeperContents
{
    std::vector<Event> visible{};
    Hlc sealed{};
    bool truncated{};
    std::optional<Hlc> own_cut{};
    std::string instance{};
    size_t limit{};
};
// Writer 4 is on a second Keeper; writer 2 and any added idle writer share one Keeper.
// Factory seeds story 1 with two writers (2,3)/(4,3), overlap duplicates,
// and an unconfirmed event at HLC {150,0} below the archive boundary 200.
// Every event lies in [100,300). setFrontiers controls each registered writer;
// failSource/truncateSource simulate Keeper failures; finishTail terminates tail.
struct ReplayHarness
{
    std::unique_ptr<Replay> sut;
    std::function<void(bool, int64_t, uint64_t)> physicalState;
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
    // Tail gates (I6.13). setKeeper replaces what keeper-a, keeper-b or the predecessor keeper-c holds and reports;
    // keeper-c exists only once it is set, with an own_cut. awaitPolls blocks until the implementation has consulted
    // its Keepers `n` more times; lastPollStart is the frontier its latest consultation started from and
    // lastSourceStart the lower bound it asked one Keeper for. abandonRange records that [start, end) was abandoned
    // (I4.15). tombstoneStory also makes every Keeper refuse the story with FAILED_PRECONDITION and the Catalog say so.
    std::function<void(const std::string&, KeeperContents)> setKeeper;
    std::function<void(unsigned)> awaitPolls;
    std::function<Hlc()> lastPollStart;
    std::function<Hlc(const std::string&)> lastSourceStart;
    std::function<void(Hlc, Hlc)> abandonRange;
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

Event TailEvent(uint64_t writer, uint64_t sequence, int64_t hlc)
{
    Event e;
    e.id = {1, writer, 3, sequence};
    e.hlc = {hlc, 0};
    e.physical.physical_ns = hlc;
    e.durability = Durability::Durable;
    return e;
}
std::vector<int64_t> HlcsOf(const std::vector<Event>& events)
{
    std::vector<int64_t> out;
    for(const auto& e: events) out.push_back(e.hlc.physical_ns);
    return out;
}
// Pulls a Tail on its own thread so a test can see that nothing has been delivered yet.
class TailPull
{
public:
    explicit TailPull(ReplayStream& stream)
        : stream_(stream)
        , worker_([this] { run(); })
    {}
    ~TailPull()
    {
        stream_.cancel();
        worker_.join();
    }
    size_t delivered()
    {
        std::lock_guard lk(mu_);
        return events_.size();
    }
    // Waits until `want` events arrived, the stream ended or `limit` passed; returns what arrived.
    std::vector<Event> get(size_t want, std::chrono::milliseconds limit = std::chrono::seconds(8))
    {
        std::unique_lock lk(mu_);
        cv_.wait_for(lk, limit, [&] { return events_.size() >= want || done_; });
        return events_;
    }
    // Waits for the stream to end by itself; false when `limit` passed first.
    bool ended(std::chrono::milliseconds limit = std::chrono::seconds(8))
    {
        std::unique_lock lk(mu_);
        return cv_.wait_for(lk, limit, [&] { return done_; });
    }
    absl::Status status()
    {
        std::lock_guard lk(mu_);
        return status_;
    }
    std::vector<Completion> completions()
    {
        std::lock_guard lk(mu_);
        return completions_;
    }

private:
    void run()
    {
        for(;;)
        {
            auto batch = stream_.next();
            std::lock_guard lk(mu_);
            if(!batch.ok() || !*batch)
            {
                if(!batch.ok())
                    status_ = batch.status();
                done_ = true;
                cv_.notify_all();
                return;
            }
            for(auto& e: (**batch).events) events_.push_back(std::move(e));
            if((**batch).completion)
                completions_.push_back(*(**batch).completion);
            cv_.notify_all();
        }
    }
    ReplayStream& stream_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Event> events_;
    std::vector<Completion> completions_;
    absl::Status status_;
    bool done_{};
    std::thread worker_;
};
Event StoryStart()
{
    Event p;
    p.id.story_id = 1;
    return p;
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

TEST_P(ReplayContract, PhysicalReadCompleteWhenEveryPhysicalFrontierPassesEnd)
{
    ASSERT_TRUE(h->physicalState);
    h->physicalState(true, 300, 0);
    auto range = Query();
    range.axis = Range::Axis::Physical;
    auto stream = h->sut->read(1, range);
    ASSERT_TRUE(stream.ok());
    auto result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->completions.size(), 1u);
    EXPECT_TRUE(result->completions[0].complete);
    h->physicalState(true, 299, 0);
    stream = h->sut->read(1, range);
    ASSERT_TRUE(stream.ok());
    result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->completions[0].reason, IncompleteReason::LaggingWriters);
}
TEST_P(ReplayContract, PhysicalRangeHonorsUncertainty)
{
    ASSERT_TRUE(h->physicalState);
    h->physicalState(true, 300, 5);
    Range range{Range::Axis::Physical, {151, 0}, {155, 0}};
    auto stream = h->sut->read(1, range);
    ASSERT_TRUE(stream.ok());
    auto result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->events.size(), 1u);
    EXPECT_EQ(result->events[0].physical.physical_ns, 150);
    EXPECT_TRUE(result->completions[0].complete);
}
TEST_P(ReplayContract, UnboundedEventMakesPhysicalReadIncomplete)
{
    ASSERT_TRUE(h->physicalState);
    h->physicalState(true, 300, UINT64_MAX);
    auto range = Query();
    range.axis = Range::Axis::Physical;
    auto stream = h->sut->read(1, range);
    ASSERT_TRUE(stream.ok());
    auto result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result->completions[0].complete);
    EXPECT_EQ(result->completions[0].reason, IncompleteReason::PhysicalAxisUnbounded);
}
TEST_P(ReplayContract, StoryWithoutPolicyIsNeverPhysicallyComplete)
{
    ASSERT_TRUE(h->physicalState);
    h->physicalState(false, 300, 0);
    auto range = Query();
    range.axis = Range::Axis::Physical;
    auto stream = h->sut->read(1, range);
    ASSERT_TRUE(stream.ok());
    auto result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result->events.empty());
    EXPECT_FALSE(result->completions[0].complete);
    EXPECT_EQ(result->completions[0].reason, IncompleteReason::PhysicalAxisUnbounded);
}
TEST_P(ReplayContract, TruncatedReadFrontierIsACompletePrefix)
{
    h->setKeeperFrontiers({{"keeper-a", 7, {160, 0}, true}, {"keeper-b", 7, {180, 0}, true}});
    h->truncateSource();
    auto stream = h->sut->read(1, Query());
    ASSERT_TRUE(stream.ok());
    auto result = Collect(**stream);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->completions.size(), 1u);
    const auto cut = result->completions[0].frontier;
    EXPECT_LE(cut, (Hlc{160, 0}));
    EXPECT_EQ(result->completions[0].reason, IncompleteReason::Truncated);
    auto prefix = Query();
    prefix.end = cut;
    stream = h->sut->read(1, prefix);
    ASSERT_TRUE(stream.ok());
    auto again = Collect(**stream);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again->events.size(), result->events.size());
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
// I6.13: a Tail delivers an event only below the lowest sealed frontier of the sources it consults.
TEST_P(ReplayContract, TailDeliversAnEventThatBecomesVisibleBelowItsCursor)
{
    ASSERT_TRUE(h->setKeeper);
    ASSERT_TRUE(h->awaitPolls);
    // Writer 2's event at 100 waits for its fsync while writer 4's event at 101 is visible, so the seal is held at 100
    // until the event is visible and moves on only then (I6.11).
    h->setKeeper("keeper-a", {{TailEvent(4, 1, 101)}, {100, 0}});
    h->setKeeper("keeper-b", {{}, {500, 0}});
    auto tail = h->sut->tail(1, StoryStart());
    ASSERT_TRUE(tail.ok());
    TailPull pull(**tail);
    h->awaitPolls(2);
    EXPECT_EQ(pull.delivered(), 0u);
    h->setKeeper("keeper-a", {{TailEvent(2, 1, 100), TailEvent(4, 1, 101)}, {500, 0}});
    EXPECT_EQ(HlcsOf(pull.get(2)), (std::vector<int64_t>{100, 101}));
    h->awaitPolls(3);
    EXPECT_EQ(pull.delivered(), 2u);
}

TEST_P(ReplayContract, TailWithholdsEventsAtOrAboveTheLowestKeeperSeal)
{
    ASSERT_TRUE(h->setKeeper);
    ASSERT_TRUE(h->awaitPolls);
    const std::vector<Event> a{TailEvent(2, 1, 110), TailEvent(2, 2, 190), TailEvent(2, 3, 200), TailEvent(2, 4, 250)};
    const std::vector<Event> b{TailEvent(4, 1, 120), TailEvent(4, 2, 149), TailEvent(4, 3, 150), TailEvent(4, 4, 160)};
    h->setKeeper("keeper-a", {a, {200, 0}});
    h->setKeeper("keeper-b", {b, {150, 0}});
    auto tail = h->sut->tail(1, StoryStart());
    ASSERT_TRUE(tail.ok());
    TailPull pull(**tail);
    h->awaitPolls(2);
    // A seal is exclusive: the event at 150 stays back with everything above it.
    EXPECT_EQ(pull.delivered(), 3u);
    h->setKeeper("keeper-b", {b, {300, 0}});
    h->awaitPolls(3);
    EXPECT_EQ(pull.delivered(), 6u);
    h->setKeeper("keeper-a", {a, {300, 0}});
    EXPECT_EQ(HlcsOf(pull.get(8)), (std::vector<int64_t>{110, 120, 149, 150, 160, 190, 200, 250}));
}

TEST_P(ReplayContract, TailDoesNotMovePastASilentKeeper)
{
    ASSERT_TRUE(h->setKeeper);
    ASSERT_TRUE(h->awaitPolls);
    ASSERT_TRUE(h->setKeeperFrontiers);
    std::vector<Event> a{TailEvent(2, 1, 110), TailEvent(2, 2, 130), TailEvent(2, 3, 140)};
    h->setKeeper("keeper-a", {a, {500, 0}});
    h->setKeeper("keeper-b", {{}, {120, 0}});
    auto tail = h->sut->tail(1, StoryStart());
    ASSERT_TRUE(tail.ok());
    TailPull pull(**tail);
    h->awaitPolls(2);
    EXPECT_EQ(pull.delivered(), 1u);
    // keeper-b falls silent while keeper-a keeps appending: the poll is dropped whole.
    h->setKeeperFrontiers({{"keeper-b", 7, {120, 0}, false}});
    a.push_back(TailEvent(2, 4, 150));
    h->setKeeper("keeper-a", {a, {500, 0}});
    h->awaitPolls(3);
    EXPECT_EQ(pull.delivered(), 1u);
    // It returns holding an event below the ones keeper-a showed meanwhile.
    h->setKeeper("keeper-b", {{TailEvent(4, 1, 125)}, {500, 0}});
    h->setKeeperFrontiers({{"keeper-b", 7, {500, 0}, true}});
    EXPECT_EQ(HlcsOf(pull.get(5)), (std::vector<int64_t>{110, 125, 130, 140, 150}));
    h->awaitPolls(3);
    EXPECT_EQ(pull.delivered(), 5u);
}

TEST_P(ReplayContract, TailBoundsItsFrontierByATruncatedSource)
{
    ASSERT_TRUE(h->setKeeper);
    ASSERT_TRUE(h->awaitPolls);
    // keeper-a's answer ends at its limit after 130, so keeper-b's events above 130 wait for the rest of it.
    std::vector<Event> a{TailEvent(2, 1, 110), TailEvent(2, 2, 120), TailEvent(2, 3, 130)};
    const std::vector<Event> b{TailEvent(4, 1, 115), TailEvent(4, 2, 125), TailEvent(4, 3, 135), TailEvent(4, 4, 140)};
    h->setKeeper("keeper-a", {a, {500, 0}, true});
    h->setKeeper("keeper-b", {b, {500, 0}});
    auto tail = h->sut->tail(1, StoryStart());
    ASSERT_TRUE(tail.ok());
    TailPull pull(**tail);
    h->awaitPolls(2);
    // The last event a truncated source returned is where its answer ends: it and everything above it stay back.
    EXPECT_EQ(pull.delivered(), 4u);
    a.push_back(TailEvent(2, 4, 132));
    h->setKeeper("keeper-a", {a, {500, 0}});
    EXPECT_EQ(HlcsOf(pull.get(8)), (std::vector<int64_t>{110, 115, 120, 125, 130, 132, 135, 140}));
    h->awaitPolls(3);
    EXPECT_EQ(pull.delivered(), 8u);
}

TEST_P(ReplayContract, TailAcrossARouteChangeDeliversEveryEventOnceInOrder)
{
    ASSERT_TRUE(h->setKeeper);
    ASSERT_TRUE(h->awaitPolls);
    // keeper-c left the Route at 150 and is read as a predecessor; its event at 100 still waits for its fsync.
    // The Route Keepers hold events of the new epoch, all above the cut.
    h->setKeeper("keeper-c", {{TailEvent(6, 2, 110), TailEvent(6, 3, 140)}, {100, 0}, false, Hlc{150, 0}});
    std::vector<Event> a{TailEvent(2, 1, 160), TailEvent(2, 2, 170)};
    h->setKeeper("keeper-a", {a, {175, 0}});
    h->setKeeper("keeper-b", {{TailEvent(4, 1, 155)}, {175, 0}});
    auto tail = h->sut->tail(1, StoryStart());
    ASSERT_TRUE(tail.ok());
    TailPull pull(**tail);
    h->awaitPolls(2);
    EXPECT_EQ(pull.delivered(), 0u);
    // The predecessor seals at its cut and shows the late event: everything below the Route seals goes out in order.
    h->setKeeper("keeper-c",
                 {{TailEvent(6, 1, 100), TailEvent(6, 2, 110), TailEvent(6, 3, 140)}, {150, 0}, false, Hlc{150, 0}});
    EXPECT_EQ(HlcsOf(pull.get(6)), (std::vector<int64_t>{100, 110, 140, 155, 160, 170}));
    a.push_back(TailEvent(2, 3, 180));
    h->setKeeper("keeper-a", {a, {400, 0}});
    h->setKeeper("keeper-b", {{TailEvent(4, 1, 155)}, {400, 0}});
    EXPECT_EQ(HlcsOf(pull.get(7)), (std::vector<int64_t>{100, 110, 140, 155, 160, 170, 180}));
    h->awaitPolls(3);
    EXPECT_EQ(pull.delivered(), 7u);
}

TEST_P(ReplayContract, TailFrontierAdvancesWithoutEvents)
{
    ASSERT_TRUE(h->setKeeper);
    ASSERT_TRUE(h->awaitPolls);
    ASSERT_TRUE(h->lastPollStart);
    h->setKeeper("keeper-c", {{}, {100, 0}, false, Hlc{150, 0}});
    h->setKeeper("keeper-a", {{}, {200, 0}});
    h->setKeeper("keeper-b", {{}, {200, 0}});
    auto tail = h->sut->tail(1, StoryStart());
    ASSERT_TRUE(tail.ok());
    TailPull pull(**tail);
    h->awaitPolls(2);
    EXPECT_EQ(h->lastPollStart(), (Hlc{100, 0}));
    // The predecessor seals at its cut and the Route Keepers move on: the next poll starts at their seal, which is
    // how a predecessor leaves the sources a Tail consults.
    h->setKeeper("keeper-c", {{}, {150, 0}, false, Hlc{150, 0}});
    h->setKeeper("keeper-a", {{}, {400, 0}});
    h->setKeeper("keeper-b", {{}, {400, 0}});
    h->awaitPolls(2);
    EXPECT_EQ(h->lastPollStart(), (Hlc{400, 0}));
    EXPECT_EQ(pull.delivered(), 0u);
}
TEST_P(ReplayContract, TailMakesProgressWhenEverySourceIsTruncated)
{
    ASSERT_TRUE(h->setKeeper);
    ASSERT_TRUE(h->awaitPolls);
    // Each answer holds two events and ends there, whatever the other source returns.
    h->setKeeper("keeper-a",
                 {{TailEvent(2, 1, 110),
                   TailEvent(2, 2, 120),
                   TailEvent(2, 3, 130),
                   TailEvent(2, 4, 140),
                   TailEvent(2, 5, 150)},
                  {500, 0},
                  false,
                  std::nullopt,
                  "",
                  2});
    h->setKeeper(
            "keeper-b",
            {{TailEvent(4, 1, 200), TailEvent(4, 2, 210), TailEvent(4, 3, 220)}, {500, 0}, false, std::nullopt, "", 2});
    auto tail = h->sut->tail(1, StoryStart());
    ASSERT_TRUE(tail.ok());
    TailPull pull(**tail);
    EXPECT_EQ(HlcsOf(pull.get(8)), (std::vector<int64_t>{110, 120, 130, 140, 150, 200, 210, 220}));
    h->awaitPolls(3);
    EXPECT_EQ(pull.delivered(), 8u);
}

TEST_P(ReplayContract, TailAsksEachKeeperFromWhereItsLastAnswerEnded)
{
    ASSERT_TRUE(h->setKeeper);
    ASSERT_TRUE(h->awaitPolls);
    ASSERT_TRUE(h->lastSourceStart);
    // keeper-a's answer covers everything below its seal and is final, so what keeper-b holds back is not fetched again.
    h->setKeeper("keeper-a", {{TailEvent(2, 1, 200), TailEvent(2, 2, 210), TailEvent(2, 3, 220)}, {500, 0}});
    h->setKeeper("keeper-b", {{}, {150, 0}});
    auto tail = h->sut->tail(1, StoryStart());
    ASSERT_TRUE(tail.ok());
    TailPull pull(**tail);
    h->awaitPolls(3);
    EXPECT_EQ(pull.delivered(), 0u);
    EXPECT_EQ(h->lastSourceStart("keeper-a"), (Hlc{500, 0}));
    EXPECT_EQ(h->lastSourceStart("keeper-b"), (Hlc{150, 0}));
    h->setKeeper("keeper-b", {{}, {600, 0}});
    EXPECT_EQ(HlcsOf(pull.get(3)), (std::vector<int64_t>{200, 210, 220}));
}

TEST_P(ReplayContract, TailAcrossAKeeperRestartHasNoGapOrDuplicate)
{
    ASSERT_TRUE(h->setKeeper);
    ASSERT_TRUE(h->awaitPolls);
    h->setKeeper("keeper-a", {{TailEvent(2, 1, 110), TailEvent(2, 2, 130)}, {500, 0}, false, std::nullopt, "a1"});
    h->setKeeper("keeper-b", {{}, {120, 0}, false, std::nullopt, "b1"});
    auto tail = h->sut->tail(1, StoryStart());
    ASSERT_TRUE(tail.ok());
    TailPull pull(**tail);
    h->awaitPolls(2);
    EXPECT_EQ(pull.delivered(), 1u);
    // keeper-a restarts: its DURABLE event at 130 is recovered, it resumes above the seal it reported, and keeper-b
    // shows an event below the one already buffered from the old instance.
    h->setKeeper("keeper-b", {{TailEvent(4, 1, 125)}, {600, 0}, false, std::nullopt, "b1"});
    h->setKeeper("keeper-a", {{TailEvent(2, 2, 130), TailEvent(2, 3, 510)}, {600, 0}, false, std::nullopt, "a2"});
    EXPECT_EQ(HlcsOf(pull.get(4)), (std::vector<int64_t>{110, 125, 130, 510}));
    h->awaitPolls(3);
    EXPECT_EQ(pull.delivered(), 4u);
}

TEST_P(ReplayContract, TailStopsAtAnAbandonedRange)
{
    ASSERT_TRUE(h->setKeeper);
    ASSERT_TRUE(h->abandonRange);
    h->setKeeper("keeper-a", {{TailEvent(2, 1, 110), TailEvent(2, 2, 130)}, {500, 0}});
    h->setKeeper("keeper-b", {{TailEvent(4, 1, 120), TailEvent(4, 2, 160), TailEvent(4, 3, 170)}, {500, 0}});
    h->abandonRange({150, 0}, {155, 0});
    auto tail = h->sut->tail(1, StoryStart());
    ASSERT_TRUE(tail.ok());
    TailPull pull(**tail);
    // Everything below the range is delivered, nothing above it, and the Tail ends where a Read would not be complete.
    ASSERT_TRUE(pull.ended());
    EXPECT_EQ(HlcsOf(pull.get(0)), (std::vector<int64_t>{110, 120, 130}));
    EXPECT_TRUE(pull.status().ok());
    auto completions = pull.completions();
    ASSERT_EQ(completions.size(), 1u);
    EXPECT_FALSE(completions[0].complete);
    EXPECT_EQ(completions[0].reason, IncompleteReason::SourceFailed);
    EXPECT_EQ(completions[0].frontier, (Hlc{150, 0}));
}

TEST_P(ReplayContract, TailEndsWhenTheStoryIsTombstoned)
{
    ASSERT_TRUE(h->setKeeper);
    ASSERT_TRUE(h->tombstoneStory);
    h->setKeeper("keeper-a", {{TailEvent(2, 1, 110)}, {500, 0}});
    h->setKeeper("keeper-b", {{}, {500, 0}});
    auto tail = h->sut->tail(1, StoryStart());
    ASSERT_TRUE(tail.ok());
    TailPull pull(**tail);
    EXPECT_EQ(HlcsOf(pull.get(1)), (std::vector<int64_t>{110}));
    // The Keepers refuse the destroyed story and the Catalog confirms it.
    h->tombstoneStory();
    ASSERT_TRUE(pull.ended());
    EXPECT_EQ(pull.status().code(), absl::StatusCode::kFailedPrecondition);
}
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(ReplayContract);
} // namespace chronolog::contract
