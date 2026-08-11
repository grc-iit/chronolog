// Unit tests for KeeperTailStore: the per-keeper last-N tail that backs the
// client playback() (tail read) path. These tests exercise the store in
// isolation (no network/daemons): build sealed StoryChunks, hand them to the
// TailStore, and check the last-N sequence/payload queries, capacity eviction,
// deferred forwarding to the extraction queue, and per-story isolation.

#include <gtest/gtest.h>

#include <string>
#include <tuple>
#include <vector>

#include <chrono_monitor.h>
#include <StoryChunk.h>

#include <KeeperTailStore.h>
#include <StoryChunkExtractionQueue.h>

namespace chl = chronolog;

// KeeperTailStore -> StoryChunkExtractionQueue use the chrono logger; initialize
// it once (errors only) so the LOG_* macros are safe during these tests.
static void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "tail_store_test_logger");
        done = true;
    }
}

// Build a sealed chunk for `sid` spanning [start,end) with `count` events at
// times first_time, first_time+1, ... (clientId=client, index=0..count-1,
// record = "<tag>#<i>"). Ownership passes to whoever ingests it.
static chl::StoryChunk* makeChunk(chl::StoryId sid,
                                  uint64_t start,
                                  uint64_t end,
                                  uint64_t first_time,
                                  int count,
                                  chl::ClientId client,
                                  std::string const& tag)
{
    auto* chunk = new chl::StoryChunk("chron", "story", sid, start, end, 64);
    for(int i = 0; i < count; i++)
    {
        chl::LogEvent ev(sid, first_time + (uint64_t)i, client, (chl::chrono_index)i, tag + "#" + std::to_string(i));
        chunk->insertEvent(ev);
    }
    return chunk;
}

// ---- empty / degenerate queries -------------------------------------------

TEST(KeeperTailStore, EmptyStoreReturnsNothing)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore tail(q, 100);

    EXPECT_TRUE(tail.getTailSequences(42, 10).empty());
    EXPECT_TRUE(tail.getTailEvents(42, {chl::EventSequence{1, 1, 1}}).empty());
}

TEST(KeeperTailStore, NRequestZeroReturnsEmpty)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore tail(q, 100);
    chl::StoryId sid = 5;
    tail.ingestSealedChunk(sid, makeChunk(sid, 1000, 2000, 1000, 5, 1, "A"));

    EXPECT_TRUE(tail.getTailSequences(sid, 0).empty());
}

TEST(KeeperTailStore, EmptyChunkIngestIsNoOp)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore tail(q, 100);
    chl::StoryId sid = 9;
    tail.ingestSealedChunk(sid, makeChunk(sid, 1000, 2000, 1000, 0, 1, "EMPTY")); // 0 events

    EXPECT_TRUE(tail.getTailSequences(sid, 10).empty());
    EXPECT_EQ(q.size(), 0);
}

// ---- single-chunk last-N --------------------------------------------------

TEST(KeeperTailStore, SingleChunkReturnsNewestNAscending)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore tail(q, 100);
    chl::StoryId sid = 7;
    tail.ingestSealedChunk(sid, makeChunk(sid, 1000, 2000, 1000, 10, 1, "A")); // times 1000..1009

    auto last3 = tail.getTailSequences(sid, 3);
    ASSERT_EQ(last3.size(), 3u);
    EXPECT_EQ(std::get<0>(last3[0]), 1007u); // newest 3 == times 1007,1008,1009
    EXPECT_EQ(std::get<0>(last3[1]), 1008u);
    EXPECT_EQ(std::get<0>(last3[2]), 1009u);
    EXPECT_LT(last3[0], last3[1]); // ascending
    EXPECT_LT(last3[1], last3[2]);
}

TEST(KeeperTailStore, NRequestExceedsAvailableReturnsAll)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore tail(q, 100);
    chl::StoryId sid = 7;
    tail.ingestSealedChunk(sid, makeChunk(sid, 1000, 2000, 1000, 5, 1, "A"));

    EXPECT_EQ(tail.getTailSequences(sid, 1000).size(), 5u);
}

// ---- payload retrieval ----------------------------------------------------

TEST(KeeperTailStore, GetTailEventsReturnsCorrectPayloads)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore tail(q, 100);
    chl::StoryId sid = 7;
    tail.ingestSealedChunk(sid, makeChunk(sid, 1000, 2000, 1000, 10, 1, "A"));

    auto seqs = tail.getTailSequences(sid, 3); // 1007,1008,1009
    auto evs = tail.getTailEvents(sid, seqs);
    ASSERT_EQ(evs.size(), 3u);
    EXPECT_EQ(evs[0].time(), 1007u);
    EXPECT_EQ(evs[0].getRecord(), "A#7");
    EXPECT_EQ(evs[2].time(), 1009u);
    EXPECT_EQ(evs[2].getRecord(), "A#9");
}

TEST(KeeperTailStore, GetTailEventsSkipsUnknownSequences)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore tail(q, 100);
    chl::StoryId sid = 7;
    tail.ingestSealedChunk(sid, makeChunk(sid, 1000, 2000, 1000, 5, 1, "A")); // 1000..1004

    std::vector<chl::EventSequence> req = {chl::EventSequence{1002, 1, 2},  // present
                                           chl::EventSequence{9999, 9, 9},  // absent
                                           chl::EventSequence{1004, 1, 4}}; // present
    auto evs = tail.getTailEvents(sid, req);
    ASSERT_EQ(evs.size(), 2u);
    EXPECT_EQ(evs[0].getRecord(), "A#2");
    EXPECT_EQ(evs[1].getRecord(), "A#4");
}

// ---- multi-chunk global tail ----------------------------------------------

TEST(KeeperTailStore, MultipleChunksFormGlobalSortedTailAcrossBoundary)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore tail(q, 100);
    chl::StoryId sid = 7;
    tail.ingestSealedChunk(sid, makeChunk(sid, 1000, 2000, 1000, 5, 1, "A")); // 1000..1004
    tail.ingestSealedChunk(sid, makeChunk(sid, 2000, 3000, 2000, 5, 1, "B")); // 2000..2004

    // last 3 across both chunks -> newest is in chunk B
    auto last3 = tail.getTailSequences(sid, 3);
    ASSERT_EQ(last3.size(), 3u);
    EXPECT_EQ(std::get<0>(last3[0]), 2002u);
    EXPECT_EQ(std::get<0>(last3[2]), 2004u);

    // last 7 spans the chunk boundary: 1003,1004 + 2000..2004
    auto last7 = tail.getTailSequences(sid, 7);
    ASSERT_EQ(last7.size(), 7u);
    EXPECT_EQ(std::get<0>(last7.front()), 1003u);
    EXPECT_EQ(std::get<0>(last7.back()), 2004u);

    auto evs = tail.getTailEvents(sid, last7);
    ASSERT_EQ(evs.size(), 7u);
    EXPECT_EQ(evs.front().getRecord(), "A#3");
    EXPECT_EQ(evs.back().getRecord(), "B#4");
}

// ---- capacity eviction + deferred extraction ------------------------------

TEST(KeeperTailStore, CapacityEvictsOldestAndForwardsFullyEvictedChunk)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore tail(q, 10); // capacity 10 events/story
    chl::StoryId sid = 7;

    tail.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 5, 1, "A")); // 100..104
    tail.ingestSealedChunk(sid, makeChunk(sid, 200, 300, 200, 5, 1, "B")); // 200..204
    EXPECT_EQ(q.size(), 0);                                                // 10 events, nothing evicted yet
    EXPECT_EQ(tail.getTailSequences(sid, 100).size(), 10u);

    tail.ingestSealedChunk(sid, makeChunk(sid, 300, 400, 300, 5, 1, "C")); // 300..304 -> 15 > 10
    // oldest 5 (all of chunk A) evicted; A fully evicted -> forwarded to queue
    EXPECT_EQ(q.size(), 1);
    auto seqs = tail.getTailSequences(sid, 100);
    ASSERT_EQ(seqs.size(), 10u);                // tail capped at 10 (B+C)
    EXPECT_EQ(std::get<0>(seqs.front()), 200u); // oldest survivor is 200
    EXPECT_EQ(std::get<0>(seqs.back()), 304u);

    // an evicted event is no longer retrievable from the tail
    EXPECT_TRUE(tail.getTailEvents(sid, {chl::EventSequence{100, 1, 0}}).empty());
}

// ---- per-story isolation --------------------------------------------------

TEST(KeeperTailStore, StoriesAreIsolated)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore tail(q, 100);
    chl::StoryId s1 = 11, s2 = 22;
    tail.ingestSealedChunk(s1, makeChunk(s1, 1000, 2000, 1000, 4, 1, "S1"));
    tail.ingestSealedChunk(s2, makeChunk(s2, 1000, 2000, 1000, 6, 1, "S2"));

    EXPECT_EQ(tail.getTailSequences(s1, 100).size(), 4u);
    EXPECT_EQ(tail.getTailSequences(s2, 100).size(), 6u);

    auto e1 = tail.getTailEvents(s1, tail.getTailSequences(s1, 1));
    ASSERT_EQ(e1.size(), 1u);
    EXPECT_EQ(e1[0].getRecord(), "S1#3");
}

// ---- archival must not depend on write volume ------------------------------
//
// Regression tests for the distributed-pipeline failure "archive read returned no
// events": a story that never fills the tail was never archived at all.
//
// Before the tail store existed, KeeperStoryPipeline handed each decayed chunk
// straight to the extraction queue, so archival was driven purely by time (chunk
// decay). Routing sealed chunks through the tail store made the ONLY forwarding
// paths capacity eviction and keeper shutdown, which silently converted archival
// from time-driven to volume-driven: below tail_capacity (65536 events per story
// by default) nothing reached the grapher while the keeper ran, so nothing was
// ever written to HDF5 and the data lived only in keeper RAM.

// Drain everything currently queued for extraction, freeing it as the extraction
// module does, and report how many chunks were waiting.
static std::size_t drainQueue(chl::StoryChunkExtractionQueue& q)
{
    std::size_t drained = 0;
    while(chl::StoryChunk* chunk = q.ejectStoryChunk())
    {
        delete chunk;
        drained++;
    }
    return drained;
}

TEST(KeeperTailStore, SealedChunkIsForwardedForArchivalOnceItsRetentionWindowPasses)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    // Capacity far above the event count, exactly as in a low-volume deployment.
    chl::KeeperTailStore store(q, 65536, /*live_tail_read=*/false, /*tail_retention_ns=*/1000);

    store.ingestSealedChunk(1, makeChunk(1, 100, 200, 100, 5, 7, "e"));
    ASSERT_EQ(drainQueue(q), 0u) << "a chunk must stay readable while it is inside the retention window";

    // Still inside the window: end_time 200 + retention 1000 = 1200.
    store.ageOutChunks(1100);
    EXPECT_EQ(drainQueue(q), 0u) << "aged out too early -- tail reads would lose recent events";

    // Past it: the chunk must now be handed over for archival even though the
    // tail is nowhere near capacity.
    store.ageOutChunks(1200);
    EXPECT_EQ(drainQueue(q), 1u) << "a sealed chunk was never forwarded for archival below tail capacity";
}

TEST(KeeperTailStore, AgedOutChunkIsNoLongerServedFromTheTail)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore store(q, 65536, /*live_tail_read=*/false, /*tail_retention_ns=*/1000);

    store.ingestSealedChunk(1, makeChunk(1, 100, 200, 100, 5, 7, "e"));
    ASSERT_EQ(store.getTailSequences(1, 10).size(), 5u);

    // That read pinned the chunk, so release takes kPinTicks ticks rather than one
    // (PhaseOneAnswerPinsItsChunkAgainstAgeOut covers the deferral itself). What
    // matters here is what happens after: ownership passes to the extraction queue
    // and the index must not keep pointing at a chunk the drain thread will free.
    for(unsigned tick = 0; tick < chl::KeeperTailStore::kPinTicks; ++tick) { store.ageOutChunks(5000); }
    EXPECT_TRUE(store.getTailSequences(1, 10).empty()) << "tail index still references a handed-over chunk";
    EXPECT_EQ(drainQueue(q), 1u);
}

// ---- phase-1 pinning (two-phase tail-read consistency) ----------------------

// The tail read is two RPCs: phase 1 answers "which sequences do you hold",
// phase 2 fetches those payloads. Nothing stops the tail from releasing those
// events in between -- and getTailEvents skips an index miss silently -- so the
// client would get a short reply that looks exactly like "the story has fewer
// events". A phase-1 answer therefore pins the chunks it named.
TEST(KeeperTailStore, PhaseOneAnswerPinsItsChunkAgainstAgeOut)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore store(q, 65536, /*live_tail_read=*/false, /*tail_retention_ns=*/1000);

    store.ingestSealedChunk(1, makeChunk(1, 100, 200, 100, 5, 7, "e"));
    std::vector<chl::EventSequence> promised = store.getTailSequences(1, 10); // phase 1
    ASSERT_EQ(promised.size(), 5u);

    // A maintenance tick lands between the two phases, well past the retention
    // window. The promised events must survive it.
    store.ageOutChunks(5000);
    EXPECT_EQ(drainQueue(q), 0u) << "a chunk promised by phase 1 was archived before phase 2 could read it";
    EXPECT_EQ(store.getTailEvents(1, promised).size(), 5u) << "phase 2 lost events that phase 1 promised";
}

TEST(KeeperTailStore, PinnedChunkIsArchivedOnceTheGraceLapses)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore store(q, 65536, /*live_tail_read=*/false, /*tail_retention_ns=*/1000);

    store.ingestSealedChunk(1, makeChunk(1, 100, 200, 100, 5, 7, "e"));
    ASSERT_EQ(store.getTailSequences(1, 10).size(), 5u);

    // A pin defers archival; it must not cancel it. Once the grace lapses the
    // chunk is handed over exactly as an unpinned one would have been.
    for(unsigned tick = 0; tick < chl::KeeperTailStore::kPinTicks; ++tick) { store.ageOutChunks(5000); }
    EXPECT_EQ(drainQueue(q), 1u) << "a pinned chunk was never archived -- the pin outlived its grace";
    EXPECT_TRUE(store.getTailSequences(1, 10).empty());
}

// The deliberate boundary of the pin: it defers age-out, which is a policy timer,
// but never capacity eviction, which is a memory bound. A client polling faster
// than pins lapse would otherwise pin the whole tail forever and grow it without
// limit. A read racing capacity eviction can therefore still be truncated -- the
// client reports that as CL_ERR_PARTIAL_RESULT rather than hiding it.
TEST(KeeperTailStore, CapacityEvictionIsNotDeferredByAPin)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore store(q, /*tail_capacity=*/2, /*live_tail_read=*/false, /*tail_retention_ns=*/1000);

    store.ingestSealedChunk(1, makeChunk(1, 100, 200, 100, 2, 7, "a"));
    ASSERT_EQ(store.getTailSequences(1, 10).size(), 2u); // pins chunk "a"

    store.ingestSealedChunk(1, makeChunk(1, 200, 300, 200, 2, 7, "b")); // pushes past capacity
    EXPECT_EQ(drainQueue(q), 1u) << "a pin must not hold the tail above tail_capacity";
    EXPECT_EQ(store.getTailSequences(1, 10).size(), 2u) << "tail must be back within capacity";
}

TEST(KeeperTailStore, AgeOutLeavesYoungerStoriesAlone)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperTailStore store(q, 65536, /*live_tail_read=*/false, /*tail_retention_ns=*/1000);

    store.ingestSealedChunk(1, makeChunk(1, 100, 200, 100, 3, 7, "old"));
    store.ingestSealedChunk(2, makeChunk(2, 4000, 5000, 4000, 3, 8, "new"));

    store.ageOutChunks(2000); // past story 1's window (1200), inside story 2's (6000)
    EXPECT_EQ(drainQueue(q), 1u) << "exactly the aged story should be forwarded";
    EXPECT_TRUE(store.getTailSequences(1, 10).empty());
    EXPECT_EQ(store.getTailSequences(2, 10).size(), 3u) << "a younger story must stay in the tail";
}
