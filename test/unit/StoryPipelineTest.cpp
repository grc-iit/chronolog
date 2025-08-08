#include "StoryPipeline.h"
#include "StoryChunkExtractionQueue.h"
#include "StoryChunkIngestionHandle.h"
#include <gtest/gtest.h>
#include <limits>

namespace chl = chronolog;

// init chronolog logger
static void initLogger()
{
    int ret = chl::chrono_monitor::initialize("console", "", spdlog::level::debug, "unit_test_logger");
    ASSERT_EQ(ret, 0);
}

static constexpr uint64_t NS = 1000000000ULL;

/* ----------------------------------
   Tests on constructor
   ---------------------------------- */

TEST(StoryPipeline_TestConstructors, testValidEmptyInit)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    EXPECT_NO_THROW({ chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1); });
}

// We set the starttime as the granularity boundary
// the pipeline should keep that starttime and allocate the three chunks of the expected size
TEST(StoryPipeline_TestConstructors, testOnBoundaryStartTime)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;

    uint64_t startNs = 9ULL * NS;// 9s
    uint16_t granS = 3;          // seconds
    uint16_t windowS = 1;        // second

    ASSERT_NO_THROW({
        chl::StoryPipeline p(q, "C", "S", 1, startNs, granS, windowS);

        // TimelineStart should be exactly 9e9
        EXPECT_EQ(p.TimelineStart(), startNs);

        // There should be three chunks of granS seconds each
        uint64_t spanNs = p.TimelineEnd() - p.TimelineStart();
        EXPECT_EQ(spanNs / NS, static_cast<uint64_t>(granS * 3));
    });
}

// Here we set the starttime as not the granularity boundary
// In this case the start time should be rounded to the previous boundary
// And again the three chunks of the expected size should be allocated as expected
TEST(StoryPipeline_TestConstructors, testNonBoundaryRounding)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;

    uint64_t startNs = 5500000000ULL;// 5.5 s
    uint16_t granS = 3;              // seconds
    uint16_t windowS = 1;            // seconds

    ASSERT_NO_THROW({
        chl::StoryPipeline p(q, "C", "S", 2, startNs, granS, windowS);

        // 5.5 % 3 = 2.5 should be rounded down to 3
        const uint64_t expectedStart = 3ULL * NS;
        EXPECT_EQ(p.TimelineStart(), expectedStart);

        // total span should be 3 chunks of granS seconds each
        uint64_t spanNs = p.TimelineEnd() - p.TimelineStart();
        EXPECT_EQ(spanNs / NS, static_cast<uint64_t>(granS * 3));
    });
}

// We set the start time as very large (near max)
// The pipeline should still allocate three chunks without overflow
TEST(StoryPipeline_TestConstructors, testHugeStoryStartTimeNoOverflow)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;

    // pick the largest possible starttime
    uint64_t max64 = std::numeric_limits<uint64_t>::max();
    uint64_t granNs = 1ULL * NS;
    uint64_t maxIntervals = max64 / granNs;
    ASSERT_GE(maxIntervals, 3ULL);

    // set starttime duch that we have space for the added three chunks within max64 limit
    uint64_t startTime = (maxIntervals - 3) * granNs;

    chl::StoryPipeline p(q, "C", "S", 3, startTime, 1, 1);
    EXPECT_EQ(p.TimelineStart(), startTime);

    // timelineEnd = startTime + 3 * granNs and <= max64
    uint64_t expectedEnd = startTime + 3 * granNs;
    EXPECT_EQ(p.TimelineEnd(), expectedEnd);
    EXPECT_LE(p.TimelineEnd(), max64);

    // exactly three chunks of size granNs
    uint64_t spanNs = p.TimelineEnd() - p.TimelineStart();
    EXPECT_EQ(spanNs / NS, 3ULL);
}


/* ----------------------------------
   Tests on getActiveIngestionHandle()
   ---------------------------------- */

// Test that a new storypipeline returns a non null ingestion handle with empty active and passive deques
TEST(StoryPipeline_TestGetActiveIngestionHandle, testNonNullHandleEmptyDeques)
{
    initLogger();

    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline pipeline(q, "C", "S", 1, 0, 1, 1);

    auto* handle = pipeline.getActiveIngestionHandle();
    ASSERT_NE(handle, nullptr);
    EXPECT_TRUE(handle->getActiveDeque().empty());
    EXPECT_TRUE(handle->getPassiveDeque().empty());
}

/* ----------------------------------
  Tests on collectIngestedEvents()
  ---------------------------------- */

// test when there is no chunk we have all empty valid queues
TEST(StoryPipeline_TestCollectIngestedEvents, testNoChunkValid)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    auto* h = p.getActiveIngestionHandle();
    ASSERT_TRUE(h->getActiveDeque().empty());
    EXPECT_NO_THROW(p.collectIngestedEvents());
    EXPECT_TRUE(h->getActiveDeque().empty());
    EXPECT_TRUE(h->getPassiveDeque().empty());
}

// Insert an empty storychunk, collectIngestedEvents should remove and delete the chunk without action
// Both the dequeues should be empty
TEST(StoryPipeline_TestCollectIngestedEvents, testSingleEmptyChunk)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    auto* h = p.getActiveIngestionHandle();
    auto* chunk = new chl::StoryChunk("C", "S", 1, 0, 1);
    h->getActiveDeque().push_back(chunk);

    p.collectIngestedEvents();
    EXPECT_TRUE(h->getActiveDeque().empty());
    EXPECT_TRUE(h->getPassiveDeque().empty());
}

// Insert a non empty story chunk
// collectIngestedEvents should merge the event into the pipeline and delete the chunk
TEST(StoryPipeline_TestCollectIngestedEvents, testSingleNonEmptyChunk)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 14, 0, 1, 1);

    auto* h = p.getActiveIngestionHandle();
    auto* chunk = new chl::StoryChunk("C", "S", 14, 0, 1);
    chunk->insertEvent(chl::LogEvent(14, 0, 0, 0, "test"));
    ASSERT_FALSE(chunk->empty());
    h->getActiveDeque().push_back(chunk);

    EXPECT_NO_THROW(p.collectIngestedEvents());
    EXPECT_TRUE(h->getActiveDeque().empty());
    EXPECT_TRUE(h->getPassiveDeque().empty());
}

// Try to push a nullptr to active queue
// Ensure that collectIngestedEvents ignores it
TEST(StoryPipeline_TestCollectIngestedEvents, testNullptrInActiveDeque)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 15, 0, 1, 1);

    auto* h = p.getActiveIngestionHandle();
    h->getActiveDeque().push_back(nullptr);

    EXPECT_NO_THROW(p.collectIngestedEvents());
    EXPECT_TRUE(h->getActiveDeque().empty());
    EXPECT_TRUE(h->getPassiveDeque().empty());
}

/* ----------------------------------
  Tests on extractDecayedStoryChunks()
  ---------------------------------- */

// We will call extract before the decay threshold and confirm that no chunks are extracted
TEST(StoryPipeline_TestExtractDecayedChunks, testEmptyExtract)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    // gran 1s, window 1s, the timeline of the pipeline is [0,3*NS)
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    p.extractDecayedStoryChunks(2 * NS - 1);
    EXPECT_EQ(q.size(), 0);
}

// We create a pipeline that will group the log events into 1s time windows
// then create a small chunk with a single event
// Now we call extractdecayedchunks at the time when it would have expired so it gets put in the extraction queue
TEST(StoryPipeline_TestExtractDecayedChunks, testExtractSmallAfterDecay)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    chl::StoryChunk c("C", "S", 1, 0, 1000);
    c.insertEvent(chl::LogEvent(1, 500, 1, 0, "test"));
    p.mergeEvents(c);

    p.extractDecayedStoryChunks(2 * NS + 1);
    EXPECT_EQ(q.size(), 1);
}


// We call extract at exactly the decay boundary
// BUG -> Gets stuck indefinitely
// TEST(StoryPipeline_TestExtractDecayedChunks, testExtractAtBoundary) {
//     initLogger();
//     chl::StoryChunkExtractionQueue q;
//     chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

//     chl::StoryChunk c("C","S",1, 0, 1000);
//     c.insertEvent(chl::LogEvent(1, 500, 1, 0, "boundary"));
//     p.mergeEvents(c);

//     // call at boundary
//     p.extractDecayedStoryChunks(2*NS); // BUG-> code gets stuck here indefinitely, should ideally return something?

//     EXPECT_EQ(q.size(), 1);
// }

// We merge two separate chunks and then extract both at once
TEST(StoryPipeline_TestExtractDecayedChunks, testExtractMultiple)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    chl::StoryChunk c1("C", "S", 1, 0, NS);
    c1.insertEvent(chl::LogEvent(1, 500'000'000, 1, 0, "first"));
    p.mergeEvents(c1);

    chl::StoryChunk c2("C", "S", 1, NS, 2 * NS);
    c2.insertEvent(chl::LogEvent(1, 1'500'000'000, 1, 1, "second"));
    p.mergeEvents(c2);

    p.extractDecayedStoryChunks(3 * NS + 1);
    EXPECT_EQ(q.size(), 2);
}

// We first extract one non empty chunk after its decay and ensure it is stashed
// Then ensure that we have two chunks left with no append so the timeline should remain the same after this
TEST(StoryPipeline_TestExtractDecayedChunks, testNoAppendAfterSingleDecay)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 2, 0, 1, 1);

    // merge one non empty chunk to ensure stash would occur if extracted
    chl::StoryChunk nonEmpty("C", "S", 2, 0, 500'000'000);
    nonEmpty.insertEvent(chl::LogEvent(2, 250'000'000, 0, 0, "test"));
    p.mergeEvents(nonEmpty);
    // move to just past that chunks decay point
    p.extractDecayedStoryChunks(2 * NS + 1);
    // we should have stashed exactly 1 chunk
    EXPECT_EQ(q.size(), 1);

    // pipeline was size 3, now 2 and since no append happened expect timelineStart=1s and timelineEnd=3s
    EXPECT_EQ(p.TimelineStart(), 1 * NS);
    EXPECT_EQ(p.TimelineEnd(), 3 * NS);
}

// We try to remove two decayed chunks at once and ensure both are stashed
// Now since the pipeline keeps at least two active chunks, it should automatically append a new empty chunk at the end
TEST(StoryPipeline_TestExtractDecayedChunks, testAppendBehaviorAfterMultiDecay)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 3, 0, 1, 1);

    // merge two small non empty chunks so they will be stashed
    chl::StoryChunk c1("C", "S", 3, 0, NS);
    c1.insertEvent(chl::LogEvent(3, 500'000'000, 0, 0, "first"));
    p.mergeEvents(c1);
    chl::StoryChunk c2("C", "S", 3, NS, 2 * NS);
    c2.insertEvent(chl::LogEvent(3, NS + 500'000'000, 0, 1, "second"));
    p.mergeEvents(c2);
    // now we move beyond the second's threshold
    // first removal threshold = 2s, second = 3s so we use 3s + 1ns
    p.extractDecayedStoryChunks(3 * NS + 1);
    // confirm that both chunks got stashed
    EXPECT_EQ(q.size(), 2);

    // now after the first removal: pipeline size went from 3 to 2 and no append
    // after second removal: pipeline size went from 2 to 1 and so there should be a new chunk appended at TimelineEnd automatically
    // so the resulting pipeline has two chunks, the original third chunk at start=2s and the appended chunk at start=3s giving endtime as 4
    EXPECT_EQ(p.TimelineStart(), 2 * NS);
    EXPECT_EQ(p.TimelineEnd(), 4 * NS);
}


// We call extract just after a future chunks starttime
// Ensure that only the decayed empty head chunks are removed and the pipeline keeps exactly two chunks without appending new
TEST(StoryPipeline_TestExtractDecayedChunks, testExtractLeavesTwo)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    // push a chunk at a future time [3s,4s)
    chl::StoryChunk c("C", "S", 1, 3 * NS, 4 * NS);
    c.insertEvent(chl::LogEvent(1, 3 * NS + NS / 2, 0, 0, "futurePush"));
    p.mergeEvents(c);

    // now we have 4 chunks where only the last one is non empty
    // we extract at just after 3s, it should remove the first two and stop before the third
    uint64_t extractTime = 3 * NS + 1;
    p.extractDecayedStoryChunks(extractTime);
    // no non empty events in the first two chunks so no stashes
    EXPECT_EQ(q.size(), 0);

    // the timeline must now be [2,4)
    EXPECT_EQ(p.TimelineStart(), 2 * NS);
    EXPECT_EQ(p.TimelineEnd(), 4 * NS);
}


/* ----------------------------------
  Tests on mergeEvents()
  ---------------------------------- */

// Merge an empty StoryChunk and ensure the pipelines time span remains same and the chunk stays empty
TEST(StoryPipeline_MergeEvents, testEmptyMerge)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 100, 0, 1, 1);
    chl::StoryChunk c("C", "S", 100, 0, NS);
    EXPECT_TRUE(c.empty());

    auto span0 = p.TimelineEnd() - p.TimelineStart();
    p.mergeEvents(c);
    EXPECT_TRUE(c.empty());
    EXPECT_EQ(p.TimelineEnd() - p.TimelineStart(), span0);
}

// We will have a chunk with the firsteventtime < timelinestarttime
// In this case the event should be succesfully prepended and timelinestart should move back to cover the event
TEST(StoryPipeline_MergeEvents, testPrependSuccess)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    // start pipeline at 2s so we can prepend back to 0
    chl::StoryPipeline p(q, "C", "S", 1, 2 * NS, 1, 1);
    EXPECT_EQ(p.TimelineStart(), 2 * NS);

    // chunk has the timeline [0,1s)
    chl::StoryChunk c("C", "S", 1, 0, NS);
    // add event at 0.5s
    c.insertEvent(chl::LogEvent(1, NS / 2, 0, 0, "test"));
    ASSERT_FALSE(c.empty());

    p.mergeEvents(c);

    // after merging a chunk that starts before TimelineStart the pipeline should have prepended back to 0
    EXPECT_EQ(p.TimelineStart(), 0);
    EXPECT_TRUE(c.empty());
}

// We have the firsteventtime > timelineend time
// in this case the event should be appended and the timelineend time should be extended
TEST(StoryPipeline_MergeEvents, testSingleAppend)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    // the chunk covers [3s, 4s)
    chl::StoryChunk c("C", "S", 1, 3 * NS, 4 * NS);
    // insert event at 3.5 s
    bool inserted = c.insertEvent(chl::LogEvent(1, 3 * NS + NS / 2, 0, 0, "test"));
    ASSERT_TRUE(inserted);
    ASSERT_FALSE(c.empty());

    p.mergeEvents(c);

    // TimelineEnd should be changed from 3 to 4
    EXPECT_EQ(p.TimelineEnd(), 4 * NS);
    EXPECT_TRUE(c.empty());
}

// We will merge multiple events and ensure the timeline increases as we append future events
TEST(StoryPipeline_MergeEvents, testMultipleAppend)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    // chunk covers [5s,6s)
    chl::StoryChunk c("C", "S", 1, 5 * NS, 6 * NS);
    bool ok = c.insertEvent(chl::LogEvent(1, 5 * NS + 200'000'000, 0, 0, "test"));
    ASSERT_TRUE(ok);
    ASSERT_FALSE(c.empty());

    p.mergeEvents(c);

    // after the merge the pipeline should be extended to 6
    EXPECT_EQ(p.TimelineEnd(), 6 * NS);
    EXPECT_TRUE(c.empty());
}

/* ----------------------------------
  Tests on prependStoryChunk() 
  ---------------------------------- */

// Basic test where there is no chunk at start and we insert valid
TEST(StoryPipeline_TestPrependStoryChunk, testSuccess)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;

    chl::StoryPipeline p(q, "C", "S", 200, 5 * NS, 1, 1);
    EXPECT_EQ(p.TimelineStart(), 5 * NS);

    uint64_t beforeStart = p.TimelineStart();
    auto it = p.prependStoryChunk();

    // now start should be 4s
    EXPECT_EQ(p.TimelineStart(), beforeStart - NS);
    // the returned iterator should match 4s
    EXPECT_EQ(it->first, beforeStart - NS);
}

/* ----------------------------------
  Tests on appendStoryChunk() 
  ---------------------------------- */

// Test a valid scenario where there is no existing chunk and we append and get valid iterator and increase pipeline size
TEST(StoryPipeline_TestAppendStoryChunk, testSuccess)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;

    chl::StoryPipeline p(q, "C", "S", 100, 0, 1, 1);

    uint64_t beforeEnd = p.TimelineEnd();
    // call the private appendStoryChunk()
    auto it = p.appendStoryChunk();

    // timelineend should move from 3 to 4 after succesful append
    EXPECT_EQ(p.TimelineEnd(), beforeEnd + NS);
    // the iterator should now point to the inserted chunk
    EXPECT_EQ(it->first, beforeEnd);
}

/* ----------------------------------
  Tests on finalize() 
  ---------------------------------- */

// Call finalize on a fresh pipeline with no pending chunks
// Ensure no stash/actions/errors
TEST(StoryPipeline_TestFinalize, testNoPendingChunks)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);
    EXPECT_NO_THROW(p.finalize());
    EXPECT_EQ(q.size(), 0);

}

// Test if finalize processes the chunk in the passive deque
// It should merge and stash exactly that chunk
TEST(StoryPipeline_TestFinalize, testOnlyPassiveDeque)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    auto* h = p.getActiveIngestionHandle();
    auto* pass = new chl::StoryChunk("C", "S", 1, 0, NS);
    pass->insertEvent(chl::LogEvent(1, NS / 2, 0, 0, "passive"));
    h->getPassiveDeque().push_back(pass);

    p.finalize();
    EXPECT_EQ(q.size(), 1);

}

// Test if finalize processes the chunk in the active deque
// It should merge and stash exactly that chunk
TEST(StoryPipeline_TestFinalize, testOnlyActiveDeque)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    auto* h = p.getActiveIngestionHandle();
    auto* act = new chl::StoryChunk("C", "S", 1, 0, NS);
    act->insertEvent(chl::LogEvent(1, NS / 2, 0, 0, "active"));
    h->getActiveDeque().push_back(act);

    p.finalize();
    EXPECT_EQ(q.size(), 1);

}

// Test if finalize handles both passive and active deques in FIFO
// It should stash the non empty chunks from passive first then active and preserve the insertion order
TEST(StoryPipeline_TestFinalize, testMixedDeques)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    auto* h = p.getActiveIngestionHandle();
    auto* pass = new chl::StoryChunk("C", "S", 1, 0, NS);
    pass->insertEvent(chl::LogEvent(1, NS / 3, 0, 0, "passive"));
    h->getPassiveDeque().push_back(pass);
    auto* act = new chl::StoryChunk("C", "S", 1, NS, 2 * NS);
    act->insertEvent(chl::LogEvent(1, NS + NS / 3, 0, 0, "active"));
    h->getActiveDeque().push_back(act);

    p.finalize();
    EXPECT_EQ(q.size(), 2);

    // Now validate the order by checking payload of the event of the chunk
    auto* firstChunk = q.ejectStoryChunk();
    ASSERT_NE(firstChunk, nullptr);
    std::vector<chl::Event> ev;
    firstChunk->extractEventSeries(ev);
    ASSERT_EQ(ev.size(), 1);
    EXPECT_EQ(ev[0].log_record(), "passive");
    delete firstChunk;

    auto* secondChunk = q.ejectStoryChunk();
    ASSERT_NE(secondChunk, nullptr);
    ev.clear();
    secondChunk->extractEventSeries(ev);
    ASSERT_EQ(ev.size(), 1);
    EXPECT_EQ(ev[0].log_record(), "active");
    delete secondChunk;
}

// Test if finalize removes all initial empty chunks from the timeline and stashes the one non empty chunk
TEST(StoryPipeline_TestFinalize, testEmptyVsNonTimeline)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    // merge a chunk at [1s–2s) so the pipeline has one non empty chunk
    chl::StoryChunk c("C", "S", 1, NS, 2 * NS);
    c.insertEvent(chl::LogEvent(1, NS + NS / 2, 0, 0, "test"));
    p.mergeEvents(c);

    p.finalize();
    EXPECT_EQ(q.size(), 1);
}

// Test calling finalize safely twice
// The first call stashes the pending chunk and second call does nothing
TEST(StoryPipeline_TestFinalize, testFinalizeDoubleCall)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    auto* h = p.getActiveIngestionHandle();
    auto* chunk = new chl::StoryChunk("C", "S", 1, 0, NS);
    chunk->insertEvent(chl::LogEvent(1, NS / 2, 0, 0, "test"));
    h->getActiveDeque().push_back(chunk);

    // first finalize should stash the one chunk
    EXPECT_NO_THROW(p.finalize());
    EXPECT_EQ(q.size(), 1);


    // second finalize should not stash anything new
    EXPECT_NO_THROW(p.finalize());
    EXPECT_EQ(q.size(), 1);
}

// finalize should stash all the non empty chunks
TEST(StoryPipeline_TestFinalize, testFinalizeWithMixedTimeline)
{
    initLogger();
    chl::StoryChunkExtractionQueue q;
    chl::StoryPipeline p(q, "C", "S", 1, 0, 1, 1);

    // merge event in the [0–1s) chunk
    chl::StoryChunk c1("C", "S", 1, 0, NS);
    c1.insertEvent(chl::LogEvent(1, NS / 4, 0, 0, "t0"));
    p.mergeEvents(c1);

    // merge event in the [1–2s) chunk
    chl::StoryChunk c2("C", "S", 1, NS, 2 * NS);
    c2.insertEvent(chl::LogEvent(1, NS + NS / 4, 1, 0, "t1"));
    p.mergeEvents(c2);

    // merge event in the [3–4s) chunk
    chl::StoryChunk c3("C", "S", 1, 3 * NS, 4 * NS);
    c3.insertEvent(chl::LogEvent(1, 3 * NS + NS / 4, 2, 0, "t3"));
    p.mergeEvents(c3);

    // all three non empty chunks should be stashed
    p.finalize();
    EXPECT_EQ(q.size(), 3);
}
