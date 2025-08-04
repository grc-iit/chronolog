#include "StoryChunk.h"
#include "chrono_monitor.h"
#include <atomic>
#include <gtest/gtest.h>
#include <map>
#include <spdlog/spdlog.h>
#include <thread>

namespace chl = chronolog;

// Helper function to assert ordering of events by time,clientid,idx
// Will be useful in future developments of storychunk which may not depend on map
static void ASSERT_EVENTS_ORDERED(const std::vector<chl::Event>& seq)
{
    for(size_t i = 1; i < seq.size(); ++i)
    {
        auto& prev = seq[i - 1];
        auto& cur = seq[i];
        // check timestamp in non decreasing order
        ASSERT_LE(prev.time(), cur.time());
        if(prev.time() == cur.time())
        {
            // if same time then client_id should be non decreasing order
            ASSERT_LE(prev.client_id(), cur.client_id());
            if(prev.client_id() == cur.client_id())
            {
                // if same client then index should be non-decreasing
                ASSERT_LE(prev.index(), cur.index());
            }
        }
    }
}


/* ----------------------------------
  Tests on the constructor
  ---------------------------------- */

TEST(StoryChunk_TestConstructors, testEmptyConstructor)
{
    chl::StoryChunk chunk;
    EXPECT_TRUE(chunk.empty());
}

TEST(StoryChunk_TestConstructors, testNormalCtorInit)
{
    int startTime = 100, endTime = 200;
    chl::StoryChunk chunk("ChronicleName", "StoryName", 1, startTime, endTime, 10);
    EXPECT_EQ(chunk.getStartTime(), startTime);
    EXPECT_EQ(chunk.getEndTime(), endTime);
}

// Test the constructor init when we have
// an endTime time less than the startTime time
// The init should be +5000 as per storychunk code
TEST(StoryChunk_TestConstructors, testBoundaryCtorInit)
{
    int startTime = 100, endTime = 50;
    chl::StoryChunk chunk("ChronicleName", "StoryName", 1, startTime, endTime, 10);
    EXPECT_EQ(chunk.getStartTime(), startTime);
    EXPECT_EQ(chunk.getEndTime(), startTime + 5000);
}


/* ----------------------------------
  Tests on insertevent()
  ---------------------------------- */

// Testing a normal insert event
TEST(StoryChunk_TestInsertEvent, testNormalInsertEvent)
{
    int storyId(1);
    int startTime = 100, endTime = 200;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    int eventTime = 150, clientId = 2, eventIdx = 9;
    std::string record = "test";
    chl::LogEvent event(storyId, eventTime, clientId, eventIdx, record);
    chunk.insertEvent(event);

    EXPECT_EQ(chunk.getEventCount(), 1);

    std::vector<chl::Event> series;
    chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);

    ASSERT_EQ(series.size(), 1);
    EXPECT_EQ(series[0].time(), eventTime);
    EXPECT_EQ(series[0].client_id(), clientId);
    EXPECT_EQ(series[0].index(), eventIdx);
    EXPECT_EQ(series[0].log_record(), record);
}

// Insert an event with the time as endTime time of the chunk
// it should get rejected
TEST(StoryChunk_TestInsertEvent, testInsertAtEndTime)
{
    int storyId(1);
    int startTime = 100, endTime = 200;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    chl::LogEvent event(storyId, endTime, 1, 1, "test");
    chunk.insertEvent(event);

    EXPECT_EQ(chunk.getEventCount(), 0);
}

// Insert an event with the time before the chunk startTime time
// it should agian get rejected
TEST(StoryChunk_TestInsertEvent, testInsertBeforeStart)
{
    int storyId(1);
    int startTime = 100, endTime = 200;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    chl::LogEvent event(storyId, startTime - 1, 1, 1, "test");
    chunk.insertEvent(event);

    EXPECT_EQ(chunk.getEventCount(), 0);
}

// try inserting an event at the exact same time as start time
// should be accepted
TEST(StoryChunk_TestInsertEvent, testInsertAtStartTime)
{
    int storyId(1);
    int startTime = 100, endTime = 200;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    chl::LogEvent event(storyId, startTime, 1, 1, "test");
    chunk.insertEvent(event);

    EXPECT_EQ(chunk.getEventCount(), 1);
}

// Insert events out of order at different timestanps
// They should be still stored chronologically by their timestamp
TEST(StoryChunk_TestInsertEvent, testOutOfOrderInsert)
{
    int storyId(1);
    int startTime = 100, endTime = 200;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    std::vector<int> timestamps = {180, 120, 150, 160, 199, 101};
    for(int t: timestamps)
    {
        chl::LogEvent ev(storyId, t, 1, 1, "test");
        EXPECT_EQ(chunk.insertEvent(ev), 1);
    }

    EXPECT_EQ(chunk.getEventCount(), timestamps.size());

    std::vector<chl::Event> series;
    chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);
    ASSERT_EQ(series.size(), timestamps.size());

    std::vector<int> expected = {101, 120, 150, 160, 180, 199};
    for(int i = 0; i < (int)expected.size(); ++i) EXPECT_EQ(series[i].time(), expected[i]);
}

// Test duplicate inserts
// We insert events with the same timestamp but different clientid and index -> both should be accepted
// at the end we insert one true duplicate which should be rejected
TEST(StoryChunk_TestInsertEvent, testDuplicateKeyCombinations)
{
    int storyId = 1;
    int startTime = 100, endTime = 200;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    // First insert: time=150, client=1, index=1
    EXPECT_EQ(chunk.insertEvent(chl::LogEvent(storyId, 150, 1, 1, "first")), 1);
    // Second insert: same time, same client, different index
    // Should be accepted
    EXPECT_EQ(chunk.insertEvent(chl::LogEvent(storyId, 150, 1, 2, "second")), 1);
    // Third insert: same time, different client, same index
    // Should be accepted
    EXPECT_EQ(chunk.insertEvent(chl::LogEvent(storyId, 150, 2, 1, "third")), 1);
    // Fourth insert: Exact duplicate of the first
    // Should be rejected
    // BUG -> this returns 1 but also rejects, shouldnt it just return 0 for failure?
    EXPECT_EQ(chunk.insertEvent(chl::LogEvent(storyId, 150, 1, 1, "duplicate")), 1);

    // We should have exactly 3 events
    EXPECT_EQ(chunk.getEventCount(), 3);

    // Validate events inserted
    std::vector<chl::Event> series;
    chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);
    ASSERT_EQ(series.size(), 3);

    // All timestamps are 150
    EXPECT_EQ(series[0].time(), 150);
    EXPECT_EQ(series[1].time(), 150);
    EXPECT_EQ(series[2].time(), 150);

    // Events should be ordered by client then index within the same time:
    EXPECT_EQ(series[0].client_id(), 1);
    EXPECT_EQ(series[0].index(), 1);
    EXPECT_EQ(series[0].log_record(), "first");

    EXPECT_EQ(series[1].client_id(), 1);
    EXPECT_EQ(series[1].index(), 2);
    EXPECT_EQ(series[1].log_record(), "second");

    EXPECT_EQ(series[2].client_id(), 2);
    EXPECT_EQ(series[2].index(), 1);
    EXPECT_EQ(series[2].log_record(), "third");
}


// Stress test on insert events by inserting many events at once
TEST(StoryChunk_TestInsertEvent, testStressInsert)
{
    int storyId(1);
    int startTime = 0;
    const int eventCount = 100000;
    int endTime = startTime + eventCount;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, eventCount);

    for(int i = 0; i < eventCount; ++i)
    {
        chl::LogEvent ev(storyId, startTime + i, i % 5, i, "stress");
        EXPECT_EQ(chunk.insertEvent(ev), 1);
    }

    // All inserts should be succesful
    EXPECT_EQ(chunk.getEventCount(), eventCount);

    // verify order and count of events
    std::vector<chl::Event> series;
    chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);
    ASSERT_EQ(series.size(), eventCount);
    for(int i = 0; i < eventCount; ++i) { EXPECT_EQ(series[i].time(), startTime + i); }

    EXPECT_TRUE(chunk.empty());
}

// Insert an event with a different story id as compared to the storychunk's storyid
// It should be rejected ideally
// BUG -> it should not insert
TEST(StoryChunk_TestInsertEvent, testIncorrectEventId)
{
    int storyId(1);
    int startTime = 100, endTime = 200;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    chl::LogEvent event(storyId + 2, startTime + 5, 1, 1, "test");
    chunk.insertEvent(event);

    EXPECT_EQ(chunk.getEventCount(), 0);
}


// Test with the max chunk size as 0 or negative
// NOTE: This will be a test for the new StoryChunk Implementation, We can ignore this for now
// TEST(StoryChunk_TestInsertEvent, testZeroChunkSize)
// {
//     int storyId(1);
//     int startTime = 100, endTime = 200;
//     chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 0);

//     int eventTime = 150, clientId = 2, eventIdx = 9;
//     std::string record = "test";
//     chl::LogEvent event(storyId, eventTime, clientId, eventIdx, record);
//     chunk.insertEvent(event);

//     EXPECT_EQ(chunk.getEventCount(), 0);
// }

// Leave the event record name as nullptr
// BUG -> Need to add a proper handling of nullptr at event creation
TEST(StoryChunk_TestInsertEvent, testNullEvent)
{
    int storyId(1);
    int startTime = 100, endTime = 200;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    int eventTime = 150, clientId = 2, eventIdx = 9;
    std::string record = "test";
    chl::LogEvent event(storyId, eventTime, clientId, eventIdx, nullptr);
    chunk.insertEvent(event);

    EXPECT_EQ(chunk.getEventCount(), 0);
}

// Insert more events than the chunksize capacity nb of events
// NOTE: This will be a test for the new StoryChunk Implementation, We can ignore this for now
// TEST(StoryChunk_TestInsertEvent, testInsertBeyondCapacity)
// {
//     int storyId(1);
//     int startTime = 100, endTime = 200;
//     int chunkCapacity = 5;
//     chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, chunkCapacity);

//     // First insert exact same nb of events as chunk capacity and should be all success
//     for(int i = 0; i < chunkCapacity; ++i)
//     {
//         chl::LogEvent event(storyId, startTime + i, i, i, "test");
//         EXPECT_EQ(chunk.insertEvent(event), 1);
//     }

//     // Now we insert one more event which should get rejected
//     chl::LogEvent extraEvent(storyId, startTime + chunkCapacity, 0, 0, "extraevent");
//     EXPECT_EQ(chunk.insertEvent(extraEvent), 0);

//     // the total event count should be same as the capacity
//     EXPECT_EQ(chunk.getEventCount(), chunkCapacity);
// }

// Insert a very large event record
TEST(StoryChunk_TestInsertEvent, testInsertLargeEventRecord)
{
    int storyId(2);
    int startTime = 0, endTime = 1000;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 1);

    std::string largePayload(20 * 1024, 'A');
    chl::LogEvent largeEv(storyId, startTime + 1, 42, 7, largePayload);

    EXPECT_EQ(chunk.insertEvent(largeEv), 1);
    EXPECT_EQ(chunk.getEventCount(), 1);

    std::vector<chl::Event> series;
    chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);
    ASSERT_EQ(series.size(), 1);

    EXPECT_EQ(series[0].log_record().size(), largePayload.size());
    EXPECT_EQ(series[0].log_record(), largePayload);
    EXPECT_TRUE(chunk.empty());
}

/* ----------------------------------
  Tests on mergeEvents()
  ---------------------------------- */

// Need to init chrono logger to avoid abort when we call mergeevents()
static void initLogger()
{
    int ret = chl::chrono_monitor::initialize("console", "", spdlog::level::debug, "unit_test_logger");
    ASSERT_EQ(ret, 0);
}

TEST(StoryChunk_TestMergeEvents, testEmptyInputMap)
{
    initLogger();

    std::map<chl::EventSequence, chl::LogEvent> events;
    auto mergeStart = events.cbegin();
    chl::StoryChunk chunk("ChronicleName", "StoryName", 1, 100, 200, 10);

    // try to merge empty event
    int retMerge = chunk.mergeEvents(events, mergeStart);
    EXPECT_EQ(retMerge, 0);
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(chunk.getEventCount(), 0);
}

// Merge events should only merge the events within the current story chunks start and end widnow
// So we create a set of events, one of them has a time before the shunks start time
// should get rejected, shouldnt be merged
TEST(StoryChunk_TestMergeEvents, testStartBeforeWindow)
{
    initLogger();

    std::map<chl::EventSequence, chl::LogEvent> events;
    int startTime = 100, endTime = 200;
    int storyId(1);
    events.insert({{50, 0, 0}, chl::LogEvent(storyId, 50, 0, 0, "eventbeforestart")});
    events.insert({{120, 1, 1}, chl::LogEvent(storyId, 120, 1, 1, "eventinbetween")});
    events.insert({{220, 0, 0}, chl::LogEvent(storyId, 50, 0, 0, "eventafterend")});

    auto mergeStart = events.cbegin();
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);
    int retMerge = chunk.mergeEvents(events, mergeStart);
    EXPECT_EQ(retMerge, 1);
    EXPECT_EQ(chunk.getEventCount(), 1);
    ASSERT_EQ(events.size(), 2);
    EXPECT_EQ(events.begin()->second.time(), 50);
}

// test merge events on a valid set of events
TEST(StoryChunk_TestMergeEvents, testValidMerge)
{
    initLogger();

    std::map<chl::EventSequence, chl::LogEvent> events;
    int startTime = 100, endTime = 200;
    int storyId(2);
    events.insert({{120, 0, 0}, chl::LogEvent(storyId, 110, 0, 0, "test")});
    events.insert({{150, 1, 1}, chl::LogEvent(storyId, 150, 1, 1, "test")});

    auto mergeStart = events.cbegin();
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    int retMerge = chunk.mergeEvents(events, mergeStart);
    EXPECT_EQ(retMerge, 2);
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(chunk.getEventCount(), 2);
}

// try to merge all invalid events in terms of time window
// all should get rejected
TEST(StoryChunk_TestMergeEvents, testAllInvalidMerges)
{
    initLogger();

    std::map<chl::EventSequence, chl::LogEvent> events;
    int startTime = 100, endTime = 200;
    int storyId(3);
    events.insert({{10, 0, 0}, chl::LogEvent(storyId, 10, 0, 0, "test")});
    events.insert({{50, 1, 1}, chl::LogEvent(storyId, 50, 1, 1, "tes")});
    events.insert({{220, 0, 0}, chl::LogEvent(storyId, 210, 0, 0, "test")});
    events.insert({{250, 1, 1}, chl::LogEvent(storyId, 250, 1, 1, "test")});
    auto mergeStart = events.cbegin();
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    int retMerge = chunk.mergeEvents(events, mergeStart);
    EXPECT_EQ(retMerge, 0);
    EXPECT_EQ(events.size(), 4);
    EXPECT_EQ(chunk.getEventCount(), 0);
}


// when the story chunk capacity has been reached the merge should stop there
// so we try to merge more events that the chunk capacity and it should stop there
// the rejected events should not be deleted from the event map
// NOTE: This will be a test for the new StoryChunk Implementation, We can ignore this for now
// TEST(StoryChunk_TestMergeEvents, testMergeMoreThanCapacity)
// {
//     initLogger();

//     std::map<chl::EventSequence, chl::LogEvent> events;
//     int startTime = 100, endTime = 200, chunkCapacity = 2;
//     int storyId(6);

//     events.insert({{110, 0, 0}, chl::LogEvent(storyId, 110, 0, 0, "test")});
//     events.insert({{120, 1, 1}, chl::LogEvent(storyId, 120, 1, 1, "test")});
//     events.insert({{150, 1, 1}, chl::LogEvent(storyId, 150, 1, 1, "test")});
//     events.insert({{160, 1, 1}, chl::LogEvent(storyId, 160, 1, 1, "test")});

//     auto mergeStart = events.cbegin();
//     chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, chunkCapacity);

//     int retMerge = chunk.mergeEvents(events, mergeStart);

//     EXPECT_EQ(retMerge, chunkCapacity);
//     EXPECT_EQ(chunk.getEventCount(), chunkCapacity);
//     ASSERT_EQ(events.size(), 2);
//     EXPECT_EQ(events.begin()->second.time(), 150);
// }

// try merging events with duplicate timestamp, all should get merged
// no rejects
TEST(StoryChunk_TestMergeEvents, testDuplicateTimestamps)
{
    initLogger();

    std::map<chl::EventSequence, chl::LogEvent> events;
    int startTime = 100, endTime = 200;
    int storyId(7);

    events.insert({{110, 0, 0}, chl::LogEvent(storyId, 110, 0, 0, "test")});
    events.insert({{110, 1, 1}, chl::LogEvent(storyId, 110, 1, 1, "test")});
    events.insert({{120, 0, 0}, chl::LogEvent(storyId, 120, 0, 0, "test")});

    auto mergeStart = events.cbegin();
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    int retMerge = chunk.mergeEvents(events, mergeStart);
    EXPECT_EQ(retMerge, 3);
    EXPECT_EQ(chunk.getEventCount(), 3);
    EXPECT_TRUE(events.empty());
}

// stress test the merge events
// try to merge a big nb of events
TEST(StoryChunk_TestMergeEvents, testHugeMerge)
{
    initLogger();

    std::map<chl::EventSequence, chl::LogEvent> events;
    int nbEvents = 1000;
    int startTime = 0, endTime = nbEvents + 10;
    int storyId(8);

    for(int i = 0; i < nbEvents; ++i) { events.insert({{i, 0, 0}, chl::LogEvent(storyId, i, 0, 0, "test")}); }

    auto mergeStart = events.cbegin();
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, nbEvents);

    int retMerge = chunk.mergeEvents(events, mergeStart);

    EXPECT_EQ(retMerge, nbEvents);
    EXPECT_EQ(chunk.getEventCount(), nbEvents);
    EXPECT_TRUE(events.empty());
}

// test merging an event at the boundary end time (endtime-1 is okay, endtime should be rejected)
TEST(StoryChunk_TestMergeEvents, BoundaryEndTimeExclusion)
{
    initLogger();

    std::map<chl::EventSequence, chl::LogEvent> events;
    int startTime = 100, endTime = 200;
    int storyId(9);

    events.insert({{100, 0, 0}, chl::LogEvent(storyId, 100, 0, 0, "test")});
    events.insert({{150, 1, 1}, chl::LogEvent(storyId, 150, 1, 1, "test")});
    events.insert({{199, 2, 2}, chl::LogEvent(storyId, 199, 2, 2, "test")});
    events.insert({{200, 3, 3}, chl::LogEvent(storyId, 200, 3, 3, "test")});

    auto mergeStart = events.cbegin();
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    int retMerge = chunk.mergeEvents(events, mergeStart);

    EXPECT_EQ(retMerge, 3);
    EXPECT_EQ(chunk.getEventCount(), 3);
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events.begin()->second.time(), 200);
}


// test merging the events from a random place in the middle of the event set instead of start
TEST(StoryChunk_TestMergeEvents, testMergeFromMiddle)
{
    initLogger();

    std::map<chl::EventSequence, chl::LogEvent> events;
    int startTime = 100, endTime = 200;
    int storyId(1);

    for(int t: {100, 110, 120, 130}) events.insert({{t, 0, 0}, chl::LogEvent(storyId, t, 0, 0, "test")});

    // update the iterator to start merging from the middle of the event set
    auto mergeStart = std::next(events.cbegin(), 2);

    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);
    int retMerge = chunk.mergeEvents(events, mergeStart);

    EXPECT_EQ(retMerge, 2);
    EXPECT_EQ(chunk.getEventCount(), 2);
    ASSERT_EQ(events.size(), 2);
    EXPECT_EQ(events.begin()->second.time(), 100);
}

// Test mergevents(chunk, time)

// try to merge an empty chunk to another main chunk
TEST(StoryChunk_TestMergeEvents, testMergeEmptyChunk)
{
    initLogger();

    chl::StoryChunk masterChunk("ChronicleName", "StoryName", 1, 100, 200, 10);
    chl::StoryChunk otherChunk("ChronicleName", "StoryName", 1, 100, 200, 10);

    int retMerge = masterChunk.mergeEvents(otherChunk, 150);
    EXPECT_EQ(retMerge, 0);
    EXPECT_TRUE(masterChunk.empty());
    EXPECT_TRUE(otherChunk.empty());
}


// test merging when the other chunk has events within main chunks time window
// should merge
TEST(StoryChunk_TestMergeEvents, testMergeWithinWindow)
{
    initLogger();

    chl::StoryChunk masterChunk("ChronicleName", "StoryName", 1, 100, 200, 10);
    chl::StoryChunk otherChunk("ChronicleName", "StoryName", 1, 100, 200, 10);

    otherChunk.insertEvent(chl::LogEvent(1, 110, 0, 0, "test"));
    otherChunk.insertEvent(chl::LogEvent(1, 150, 0, 0, "test"));

    int retMerge = masterChunk.mergeEvents(otherChunk, 100);
    EXPECT_EQ(retMerge, 2);
    EXPECT_EQ(masterChunk.getEventCount(), 2);
    EXPECT_TRUE(otherChunk.empty());
}

// test merging when the other chunk has events out of main chunks time window
// other chunk should not get emptied, should just get rejected in merge to main
TEST(StoryChunk_TestMergeEvents, testMergeOutsideWindow)
{
    initLogger();

    chl::StoryChunk masterChunk("ChronicleName", "StoryName", 1, 100, 200, 10);
    chl::StoryChunk otherChunk("ChronicleName", "StoryName", 1, 0, 300, 10);

    otherChunk.insertEvent(chl::LogEvent(1, 50, 0, 0, "test"));
    otherChunk.insertEvent(chl::LogEvent(1, 150, 0, 0, "test"));
    otherChunk.insertEvent(chl::LogEvent(1, 250, 0, 0, "test"));

    int retMerge = masterChunk.mergeEvents(otherChunk, 0);
    EXPECT_EQ(retMerge, 1);
    EXPECT_EQ(masterChunk.getEventCount(), 1);
    EXPECT_EQ(otherChunk.getEventCount(), 2);
}

// test a mixed merge of other chunk
// the other chunk has a window different from main chunk
// so the main chunk should only merge the events within its accepted time frame
TEST(StoryChunk_TestMergeEvents, testMixedChunkMerge)
{
    initLogger();

    chl::StoryChunk masterChunk("ChronicleName", "StoryName", 1, 100, 200, 10);
    chl::StoryChunk otherChunk("ChronicleName", "StoryName", 1, 50, 250, 10);

    otherChunk.insertEvent(chl::LogEvent(1, 80, 0, 0, "testBefore"));
    otherChunk.insertEvent(chl::LogEvent(1, 110, 0, 0, "testIn"));
    otherChunk.insertEvent(chl::LogEvent(1, 180, 0, 0, "testIn"));
    otherChunk.insertEvent(chl::LogEvent(1, 220, 0, 0, "testAfter"));

    int retMerge = masterChunk.mergeEvents(otherChunk, 50);
    EXPECT_EQ(retMerge, 2);
    EXPECT_EQ(masterChunk.getEventCount(), 2);

    std::vector<chl::Event> leftOtherChunk;
    otherChunk.extractEventSeries(leftOtherChunk);
    ASSERT_EQ(leftOtherChunk.size(), 2);
    EXPECT_EQ(leftOtherChunk[0].time(), 80);
    EXPECT_EQ(leftOtherChunk[1].time(), 220);
}


// try calling merge events with a merge start time as after end time of the chunk
// BUG -> should not merge after end time of the chunk, should have got rejected
TEST(StoryChunk_TestMergeEvents, testMergeStartTimeAfterEnd)
{
    initLogger();
    chl::StoryChunk masterChunk("ChronicleName", "StoryName", 1, 0, 100, 10);
    chl::StoryChunk otherChunk("ChronicleName", "StoryName", 1, 0, 100, 10);
    otherChunk.insertEvent(chl::LogEvent(1, 10, 0, 0, "test"));

    int retMerge = masterChunk.mergeEvents(otherChunk, 200);
    EXPECT_EQ(retMerge, 0);
    EXPECT_EQ(masterChunk.getEventCount(), 0);
    EXPECT_EQ(otherChunk.getEventCount(), 1);
}

// test merging a map of events to the main chunk
// then merge another chunk to the main chunk, it should all be okay
TEST(StoryChunk_TestMergeEvents, testMergeMapThenChunk)
{
    initLogger();

    // first create map of events
    int storyId(1);
    std::map<chl::EventSequence, chl::LogEvent> events;
    events.insert({{110, 0, 0}, chl::LogEvent(storyId, 110, 0, 0, "test")});
    events.insert({{120, 0, 0}, chl::LogEvent(storyId, 120, 0, 0, "test")});
    auto it = events.cbegin();
    // mow merge the map to masterchunk
    chl::StoryChunk masterChunk("ChronicleName", "StoryName", storyId, 100, 200, 10);
    int merged1 = masterChunk.mergeEvents(events, it);
    EXPECT_EQ(merged1, 2);
    EXPECT_EQ(masterChunk.getEventCount(), 2);

    // now create another storychunk with some other events
    chl::StoryChunk otherChunk("ChronicleName", "StoryName", storyId, 100, 200, 10);
    otherChunk.insertEvent(chl::LogEvent(storyId, 130, 0, 0, "test"));
    EXPECT_EQ(otherChunk.getEventCount(), 1);

    // now merge the otherchunk to this master chunk
    int merged2 = masterChunk.mergeEvents(otherChunk, 0);
    EXPECT_EQ(merged2, 1);
    EXPECT_EQ(masterChunk.getEventCount(), 3);

    //  confirm now that other chunk is empty
    EXPECT_TRUE(otherChunk.empty());
}


// try merging an event with a different story id than the chunk storyid
// BUG -> incorrect storyid should not be allowed
TEST(StoryChunk_TestMergeEvents, testIncorrectStoryId)
{
    initLogger();

    std::map<chl::EventSequence, chl::LogEvent> events;
    events.insert({{110, 0, 0}, chl::LogEvent(99, 110, 0, 0, "wrong")});
    auto it = events.cbegin();
    chl::StoryChunk chunk("ChronicleName", "StoryName", 1, 100, 200, 10);

    int retMerge = chunk.mergeEvents(events, it);
    EXPECT_EQ(retMerge, 0);
    EXPECT_EQ(events.size(), 1);
}

// test merging an empty event and huge payload
// both should merge
TEST(StoryChunk_TestMergeEvents, testEmptyAndHugePayloads)
{
    initLogger();

    std::map<chl::EventSequence, chl::LogEvent> events;
    int storyId(3);
    events.insert({{110, 0, 0}, chl::LogEvent(storyId, 110, 0, 0, "")});
    events.insert({{120, 0, 0}, chl::LogEvent(storyId, 120, 0, 0, std::string(5000, 't'))});

    auto it = events.cbegin();
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 100, 200, 10);
    int retMerge = chunk.mergeEvents(events, it);

    EXPECT_EQ(retMerge, 2);
    EXPECT_EQ(chunk.getEventCount(), 2);
}


// other starts before main and ends inside main
// again only the events in the time of main merge
TEST(StoryChunk_TestMergeEvents, testMergeStartBeforeEndInside)
{
    initLogger();
    chl::StoryChunk main("ChronicleName", "StoryName", 1, 100, 200, 10),
            other("ChronicleName", "StoryName", 1, 50, 150, 10);
    other.insertEvent({1, 60, 0, 0, "BeforeMain"});
    other.insertEvent({1, 120, 0, 0, "InMain"});
    other.insertEvent({1, 140, 0, 0, "InMain"});
    EXPECT_EQ(other.getEventCount(), 3);

    int merged = main.mergeEvents(other, other.getStartTime());
    EXPECT_EQ(merged, 2);
    EXPECT_EQ(main.getEventCount(), 2);
    EXPECT_EQ(other.getEventCount(), 1);

    std::vector<chl::Event> left;
    other.extractEventSeries(left);
    EXPECT_EQ(left[0].time(), 60);
}

// other starts inside main but ends after main
TEST(StoryChunk_TestMergeEvents, testMergeStartInsideEndAfter)
{
    initLogger();
    chl::StoryChunk main("ChronicleName", "StoryName", 1, 100, 200, 10),
            other("ChronicleName", "StoryName", 1, 150, 250, 10);
    other.insertEvent({1, 160, 0, 0, "InsideMain"});
    other.insertEvent({1, 180, 0, 0, "InsideMain"});
    other.insertEvent({1, 220, 0, 0, "AfterMain"});
    EXPECT_EQ(other.getEventCount(), 3);

    int merged = main.mergeEvents(other, other.getStartTime());
    EXPECT_EQ(merged, 2);
    EXPECT_EQ(main.getEventCount(), 2);
    EXPECT_EQ(other.getEventCount(), 1);

    std::vector<chl::Event> left;
    other.extractEventSeries(left);
    EXPECT_EQ(left[0].time(), 220);
}

// Other starts at main's end time at boundary
// None of the events should merge in main
TEST(StoryChunk_TestMergeEvents, testMergeExactBoundaryNoOverlap)
{
    initLogger();
    chl::StoryChunk main("ChronicleName", "StoryName", 1, 100, 200, 10),
            other("ChronicleName", "StoryName", 1, 200, 300, 10);
    other.insertEvent({1, 200, 0, 0, "Boundary"});
    other.insertEvent({1, 201, 0, 0, "AfterBoundary"});
    int merged = main.mergeEvents(other, other.getStartTime());
    // Neither should merge in main
    EXPECT_EQ(merged, 0);
    EXPECT_EQ(main.getEventCount(), 0);
    EXPECT_EQ(other.getEventCount(), 2);
}

/* ----------------------------------
  Tests on eraseEvents()
  ---------------------------------- */

// We first insert three valid events and then erase a valid event at a particular timestamp
TEST(StoryChunk_TestEraseEvents, testValidErase)
{
    int storyId = 1;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 100, 250, 10);
    chunk.insertEvent(chl::LogEvent(storyId, 100, 0, 0, "test"));
    chunk.insertEvent(chl::LogEvent(storyId, 150, 0, 0, "test"));
    chunk.insertEvent(chl::LogEvent(storyId, 200, 0, 0, "test"));

    chunk.eraseEvents(150, 151);

    EXPECT_EQ(chunk.getEventCount(), 2);
    std::vector<chl::Event> series;
    chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);
    ASSERT_EQ(series.size(), 2);
    EXPECT_EQ(series[0].time(), 100);
    EXPECT_EQ(series[1].time(), 200);
}


// Call erase events at a timestamp that does not exist in the events
// others should not be touched
TEST(StoryChunk_TestEraseEvents, testNonExistingTimestamp)
{
    int storyId = 1;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 0, 100, 10);
    chunk.insertEvent(chl::LogEvent(storyId, 10, 0, 0, "test"));
    chunk.insertEvent(chl::LogEvent(storyId, 90, 0, 0, "test"));
    EXPECT_EQ(chunk.getEventCount(), 2);

    chunk.eraseEvents(50, 51);
    EXPECT_EQ(chunk.getEventCount(), 2);
}

// erase at a timestamp with two events at the same time
// both should be deleted and other should not be touched
TEST(StoryChunk_TestEraseEvents, testEraseDuplicateTimestamps)
{
    int storyId = 1;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 0, 200, 10);
    chunk.insertEvent(chl::LogEvent(storyId, 150, 0, 0, "test"));
    chunk.insertEvent(chl::LogEvent(storyId, 150, 1, 1, "test"));
    chunk.insertEvent(chl::LogEvent(storyId, 100, 0, 0, "test"));
    EXPECT_EQ(chunk.getEventCount(), 3);

    chunk.eraseEvents(150, 151);
    EXPECT_EQ(chunk.getEventCount(), 1);
    std::vector<chl::Event> series;
    chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);
    ASSERT_EQ(series.size(), 1);
    EXPECT_EQ(series[0].time(), 100);
}

// erase events in a particular time range and ensure all in that timestamp range erase
TEST(StoryChunk_TestEraseEvents, testEraseInRange)
{
    int storyId = 1;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 0, 300, 10);
    for(int t: {100, 120, 140, 160, 180}) chunk.insertEvent(chl::LogEvent(storyId, t, 0, 0, "test"));
    EXPECT_EQ(chunk.getEventCount(), 5);

    chunk.eraseEvents(120, 160);
    EXPECT_EQ(chunk.getEventCount(), 2);
    std::vector<chl::Event> series;
    chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);
    ASSERT_EQ(series.size(), 2);
    EXPECT_EQ(series[0].time(), 100);
    EXPECT_EQ(series[1].time(), 180);
}

// erase events out of the time range of the story chunk
TEST(StoryChunk_TestEraseEvents, testEraseOutOfTime)
{
    int storyId = 1;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 0, 200, 10);
    for(int t: {50, 150, 250}) chunk.insertEvent(chl::LogEvent(storyId, t, 0, 0, ""));
    EXPECT_EQ(chunk.getEventCount(), 2);

    chunk.eraseEvents(250, 300);
    EXPECT_EQ(chunk.getEventCount(), 2);
}

// call erase events with an invalid input of start greater than end
TEST(StoryChunk_TestEraseEvents, testEraseInvalidTime)
{
    int storyId = 1;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 0, 100, 10);
    chunk.insertEvent(chl::LogEvent(storyId, 10, 0, 0, "test"));
    EXPECT_EQ(chunk.getEventCount(), 1);

    chunk.eraseEvents(200, 50);
    EXPECT_EQ(chunk.getEventCount(), 1);
}

// test to erase all events
TEST(StoryChunk_TestEraseEvents, testEraseAllEvents)
{
    int storyId = 1;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 10, 1000, 10);
    for(int t = 250; t <= 1000; t += 250) chunk.insertEvent(chl::LogEvent(storyId, t, 0, 0, "test"));
    EXPECT_EQ(chunk.getEventCount(), 3);

    chunk.eraseEvents(10, 2000);
    EXPECT_TRUE(chunk.empty());
}

// insert 100k events and erase a small subset, ensure only those erase
TEST(StoryChunk_TestEraseEvents, testStressErase)
{
    int storyId = 1;
    const int N = 100000;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 0, N, N);
    for(int i = 0; i < N; ++i) chunk.insertEvent(chl::LogEvent(storyId, i, 0, 0, "test"));
    EXPECT_EQ(chunk.getEventCount(), N);

    chunk.eraseEvents(40000, 60000);
    EXPECT_EQ(chunk.getEventCount(), N - 1 - (60000 - 40000));
}

TEST(StoryChunk_TestEraseEvents, testEraseEndTimeMinusOne)
{
    int storyId = 1;
    int startTime = 100, endTime = 200;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, 10);

    chunk.insertEvent(chl::LogEvent(storyId, 150, 0, 0, "test"));
    chunk.insertEvent(chl::LogEvent(storyId, endTime - 1, 0, 0, "test"));
    chunk.insertEvent(chl::LogEvent(storyId, 180, 0, 0, "test"));
    EXPECT_EQ(chunk.getEventCount(), 3);

    chunk.eraseEvents(endTime - 1, endTime);

    EXPECT_EQ(chunk.getEventCount(), 2);
    std::vector<chl::Event> series;
    chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);
    ASSERT_EQ(series.size(), 2);
    EXPECT_EQ(series[0].time(), 150);
    EXPECT_EQ(series[1].time(), 180);
}

TEST(StoryChunk_TestEraseEvents, testZeroRange)
{
    int storyId(1);
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 0, 100, 5);
    chunk.insertEvent(chl::LogEvent(storyId, 20, 0, 0, "test"));
    chunk.insertEvent(chl::LogEvent(storyId, 40, 0, 0, "test"));
    EXPECT_EQ(chunk.getEventCount(), 2);

    chunk.eraseEvents(40, 40);
    EXPECT_EQ(chunk.getEventCount(), 2);
}


/* ----------------------------------
  Tests on extractEventSeries()
  ---------------------------------- */

TEST(StoryChunk_TestExtractEventSeries, testExtractEmptyChunk)
{
    chl::StoryChunk chunk;
    std::vector<chl::Event> series;
    auto& ret = chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);
    EXPECT_EQ(&ret, &series);
    EXPECT_TRUE(series.empty());
    EXPECT_TRUE(chunk.empty());
    EXPECT_EQ(chunk.getEventCount(), 0);
}

TEST(StoryChunk_TestExtractEventSeries, testExtractSingleEvent)
{
    int storyId = 1;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 0, 100, 10);
    chunk.insertEvent(chl::LogEvent(storyId, 25, 5, 2, "test"));
    EXPECT_EQ(chunk.getEventCount(), 1);

    std::vector<chl::Event> series;
    chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);
    ASSERT_EQ(series.size(), 1);
    EXPECT_EQ(series[0].time(), 25);
    EXPECT_EQ(series[0].client_id(), 5);
    EXPECT_EQ(series[0].index(), 2);
    EXPECT_EQ(series[0].log_record(), "test");

    EXPECT_TRUE(chunk.empty());
    EXPECT_EQ(chunk.getEventCount(), 0);
}

TEST(StoryChunk_TestExtractEventSeries, testExtractEventsSorted)
{
    int storyId = 1;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 0, 100, 10);
    for(auto t: {30, 10, 20, 15, 11, 35, 23}) chunk.insertEvent(chl::LogEvent(storyId, t, 0, 0, "test"));
    EXPECT_EQ(chunk.getEventCount(), 7);

    std::vector<chl::Event> series;
    chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);
    ASSERT_EQ(series.size(), 7);
    EXPECT_EQ(series[0].time(), 10);
    EXPECT_EQ(series[1].time(), 11);
    EXPECT_EQ(series[2].time(), 15);
    EXPECT_EQ(series[3].time(), 20);
    EXPECT_EQ(series[4].time(), 23);
    EXPECT_EQ(series[5].time(), 30);
    EXPECT_EQ(series[6].time(), 35);
}

// extract events once and then extract again and confirm if empty second time
TEST(StoryChunk_TestExtractEventSeries, testExtractTwice)
{
    int storyId = 1;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 0, 100, 10);
    chunk.insertEvent(chl::LogEvent(storyId, 50, 1, 1, "test"));
    EXPECT_EQ(chunk.getEventCount(), 1);

    std::vector<chl::Event> s1;
    chunk.extractEventSeries(s1);
    ASSERT_EVENTS_ORDERED(s1);
    ASSERT_EQ(s1.size(), 1);

    EXPECT_TRUE(chunk.empty());

    std::vector<chl::Event> s2;
    chunk.extractEventSeries(s2);
    ASSERT_EVENTS_ORDERED(s2);
    EXPECT_TRUE(s2.empty());
}

TEST(StoryChunk_TestExtractEventSeries, testExtractAfterReinsert)
{
    int storyId = 1;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 0, 100, 10);

    chunk.insertEvent(chl::LogEvent(storyId, 5, 0, 0, "one"));
    std::vector<chl::Event> s1;
    chunk.extractEventSeries(s1);
    ASSERT_EVENTS_ORDERED(s1);
    ASSERT_EQ(s1.size(), 1);
    EXPECT_EQ(s1[0].log_record(), "one");

    chunk.insertEvent(chl::LogEvent(storyId, 15, 0, 0, "two"));
    std::vector<chl::Event> s2;
    chunk.extractEventSeries(s2);
    ASSERT_EVENTS_ORDERED(s2);
    ASSERT_EQ(s2.size(), 1);
    EXPECT_EQ(s2[0].log_record(), "two");
    EXPECT_TRUE(chunk.empty());
}

TEST(StoryChunk_TestExtractEventSeries, testExtractLargePayload)
{
    int storyId = 1;
    chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, 0, 1000, 10);
    std::string big(5000, 't');
    chunk.insertEvent(chl::LogEvent(storyId, 100, 0, 0, big));

    std::vector<chl::Event> series;
    chunk.extractEventSeries(series);
    ASSERT_EVENTS_ORDERED(series);
    ASSERT_EQ(series.size(), 1);
    EXPECT_EQ(series[0].log_record().size(), big.size());
}


/* ---------------------------------------------------
  Tests on Thread Safety - NOTE: Out of scope for now
  ---------------------------------------------------- */

// We spawn "nbThreads" threads and each thread inserts "nbEventsPerThread" events
// At the end we should have exactly N*M events in the chunk
// TEST(StoryChunk_ThreadSafety, concurrentInserts)
// {
//     int storyId = 1;
//     int startTime = 0, endTime = 1000000;
//     int capacity = 10000;
//     chl::StoryChunk chunk("ChronicleName", "StoryName", storyId, startTime, endTime, capacity);

//     // we expect a total 2000 inserts at the end
//     int nbThreads = 4;
//     int nbEventsPerThread = 500;
//     std::vector<std::thread> threads;
//     threads.reserve(nbThreads);

//     for(int t = 0; t < nbThreads; ++t)
//     {
//         threads.emplace_back([&, t]() {
//             for(int i = 0; i < nbEventsPerThread; ++i)
//             {
//                 int ts = startTime + (t * nbEventsPerThread + i);
//                 chl::LogEvent ev(storyId, ts, t, i, "concurrentInsert");
//                 EXPECT_EQ(chunk.insertEvent(ev), 1);
//             }
//         });
//     }

//     for(auto& th: threads) th.join();

//     EXPECT_EQ(chunk.getEventCount(), nbThreads * nbEventsPerThread);
// }

// In future for thread safety add:
// 1. test a concurrent insert and extract test, one thread inserts and another extracts
//    expect no crash/failure and all total match correct at the end
// 2. test two threads merging two different source chunks into a common main chunk at the same time
//    at the end we should have the correct total nb of events
// 3. have a main thread insert events into a chunk, have multiple threads simultaneously extracting events
//    finally the total nb of extracted should be equal to total nb of inserted
// 4. Do similar as nb 3 for erase events by separate threads