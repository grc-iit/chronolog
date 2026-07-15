// Unit tests for KeeperChunkRetentionStore: the keeper-side owner of every
// sealed StoryChunk. A sealed chunk is indexed into the per-story last-N tail
// (backing the client playback() tail-read path) AND immediately stashed to the
// extraction queue (ship-on-seal). The store frees a chunk only when the single
// free condition holds:
//
//   shipped (grapher acked)  AND  endTime <= known W  AND  tail released
//   AND the chunk pointer is not sitting in the extraction queue
//
// These tests exercise the store in isolation (no network/daemons): build
// sealed StoryChunks, hand them to the store, drive the drain callbacks
// (markShipped/markSendFailed), watermark reports (confirmPersisted), stall
// re-sends (requeueStalled), and the tail queries.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <tuple>
#include <vector>

#include <chrono_monitor.h>
#include <StoryChunk.h>

#include <KeeperChunkRetentionStore.h>
#include <StoryChunkExtractionQueue.h>

namespace chl = chronolog;

// The store and StoryChunkExtractionQueue use the chrono logger; initialize
// it once (errors only) so the LOG_* macros are safe during these tests.
static void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "retention_store_test_logger");
        done = true;
    }
}

// Build a sealed chunk for `sid` spanning [start,end) with `count` events at
// times first_time, first_time+1, ... (clientId=client, index=0..count-1,
// record = "<tag>#<i>"). Ownership passes to whoever ingests it.
static chl::StoryChunk* makeChunk(chl::StoryId sid, uint64_t start, uint64_t end, uint64_t first_time, int count,
                                  chl::ClientId client, std::string const& tag)
{
    auto* chunk = new chl::StoryChunk("chron", "story", sid, start, end, 64);
    for(int i = 0; i < count; i++)
    {
        chl::LogEvent ev(sid, first_time + (uint64_t)i, client, (chl::chrono_index)i, tag + "#" + std::to_string(i));
        chunk->insertEvent(ev);
    }
    return chunk;
}

// Simulate one full drain iteration for the oldest stashed chunk: eject the
// pointer from the queue and deliver the transfer outcome to the store.
static chl::StoryChunk* drainOne(chl::StoryChunkExtractionQueue& q, chl::KeeperChunkRetentionStore& store,
                                 bool transfer_ok)
{
    chl::StoryChunk* chunk = q.ejectStoryChunk();
    if(chunk == nullptr)
    {
        return nullptr;
    }
    if(transfer_ok)
    {
        store.markShipped(chunk);
    }
    else
    {
        store.markSendFailed(chunk);
    }
    return chunk;
}

// ---- empty / degenerate queries -------------------------------------------

TEST(KeeperChunkRetentionStore, EmptyStoreReturnsNothing)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);

    EXPECT_TRUE(store.getTailSequences(42, 10).empty());
    EXPECT_TRUE(store.getTailEvents(42, {chl::EventSequence{1, 1, 1}}).empty());
    EXPECT_EQ(store.retainedChunkCount(42), 0u);
    EXPECT_EQ(store.knownPersisted(42), 0u);
}

TEST(KeeperChunkRetentionStore, NRequestZeroReturnsEmpty)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 1000, 1100, 1000, 5, 1, "A"));

    EXPECT_TRUE(store.getTailSequences(sid, 0).empty());
}

TEST(KeeperChunkRetentionStore, EmptyChunkIngestIsNoOp)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, new chl::StoryChunk("c", "s", sid, 1000, 1100, 64));

    EXPECT_TRUE(store.getTailSequences(sid, 10).empty());
    EXPECT_EQ(q.size(), 0);
    EXPECT_EQ(store.retainedChunkCount(sid), 0u);
}

// ---- ship-on-seal ----------------------------------------------------------

TEST(KeeperChunkRetentionStore, IngestStashesChunkToExtractionQueueOnSeal)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    auto* chunk = makeChunk(sid, 1000, 1100, 1000, 5, 1, "A");
    store.ingestSealedChunk(sid, chunk);

    ASSERT_EQ(q.size(), 1);
    EXPECT_EQ(q.ejectStoryChunk(), chunk); // same pointer, non-owning
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);
    store.markSendFailed(chunk); // close the drain protocol so nothing dangles
}

// ---- tail queries (unchanged semantics) ------------------------------------

TEST(KeeperChunkRetentionStore, SingleChunkReturnsNewestNAscending)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 1000, 1100, 1000, 10, 1, "A"));

    auto last3 = store.getTailSequences(sid, 3);
    ASSERT_EQ(last3.size(), 3u);
    EXPECT_EQ(std::get<0>(last3[0]), 1007u); // newest 3 == times 1007,1008,1009
    EXPECT_EQ(std::get<0>(last3[1]), 1008u);
    EXPECT_EQ(std::get<0>(last3[2]), 1009u);
    EXPECT_LT(last3[0], last3[1]); // ascending
    EXPECT_LT(last3[1], last3[2]);
}

TEST(KeeperChunkRetentionStore, NRequestExceedsAvailableReturnsAll)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 1000, 1100, 1000, 5, 1, "A"));

    EXPECT_EQ(store.getTailSequences(sid, 1000).size(), 5u);
}

TEST(KeeperChunkRetentionStore, GetTailEventsReturnsCorrectPayloads)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 1000, 1100, 1000, 10, 1, "A"));

    auto seqs = store.getTailSequences(sid, 3);
    auto evs = store.getTailEvents(sid, seqs);
    ASSERT_EQ(evs.size(), 3u);
    EXPECT_EQ(evs[0].time(), 1007u);
    EXPECT_EQ(evs[0].getRecord(), "A#7");
    EXPECT_EQ(evs[2].time(), 1009u);
    EXPECT_EQ(evs[2].getRecord(), "A#9");
}

TEST(KeeperChunkRetentionStore, GetTailEventsSkipsUnknownSequences)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 1000, 1100, 1000, 5, 1, "A"));

    std::vector<chl::EventSequence> asks = {chl::EventSequence{1002, 1, 2},
                                            chl::EventSequence{9999, 9, 9}, // unknown
                                            chl::EventSequence{1004, 1, 4}};
    auto evs = store.getTailEvents(sid, asks);
    ASSERT_EQ(evs.size(), 2u);
    EXPECT_EQ(evs[0].getRecord(), "A#2");
    EXPECT_EQ(evs[1].getRecord(), "A#4");
}

TEST(KeeperChunkRetentionStore, MultipleChunksFormGlobalSortedTailAcrossBoundary)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 1000, 2000, 1000, 5, 1, "A"));
    store.ingestSealedChunk(sid, makeChunk(sid, 2000, 3000, 2000, 5, 2, "B"));

    auto last3 = store.getTailSequences(sid, 3);
    ASSERT_EQ(last3.size(), 3u);
    EXPECT_EQ(std::get<0>(last3[0]), 2002u);
    EXPECT_EQ(std::get<0>(last3[2]), 2004u);

    auto last7 = store.getTailSequences(sid, 7);
    ASSERT_EQ(last7.size(), 7u);
    EXPECT_EQ(std::get<0>(last7.front()), 1003u);
    EXPECT_EQ(std::get<0>(last7.back()), 2004u);

    auto evs = store.getTailEvents(sid, last7);
    ASSERT_EQ(evs.size(), 7u);
    EXPECT_EQ(evs.front().getRecord(), "A#3");
    EXPECT_EQ(evs.back().getRecord(), "B#4");
}

TEST(KeeperChunkRetentionStore, StoriesAreIsolated)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId s1 = 1, s2 = 2;
    store.ingestSealedChunk(s1, makeChunk(s1, 1000, 2000, 1000, 4, 1, "S1"));
    store.ingestSealedChunk(s2, makeChunk(s2, 1000, 2000, 1000, 6, 2, "S2"));

    EXPECT_EQ(store.getTailSequences(s1, 100).size(), 4u);
    EXPECT_EQ(store.getTailSequences(s2, 100).size(), 6u);

    auto e1 = store.getTailEvents(s1, {chl::EventSequence{1003, 1, 3}});
    ASSERT_EQ(e1.size(), 1u);
    EXPECT_EQ(e1[0].getRecord(), "S1#3");
}

// ---- tail capacity: eviction no longer frees or re-stashes -----------------

TEST(KeeperChunkRetentionStore, CapacityEvictionKeepsChunkRetainedUntilDurable)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 10); // capacity: 10 events
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 10, 1, "A")); // fills the tail
    EXPECT_EQ(q.size(), 1); // ship-on-seal
    EXPECT_EQ(store.getTailSequences(sid, 100).size(), 10u);

    // B and C evict all of A's events from the tail index...
    store.ingestSealedChunk(sid, makeChunk(sid, 200, 300, 200, 5, 1, "B"));
    store.ingestSealedChunk(sid, makeChunk(sid, 300, 400, 300, 5, 1, "C"));
    EXPECT_EQ(q.size(), 3); // one stash per seal; eviction adds none

    auto seqs = store.getTailSequences(sid, 100);
    ASSERT_EQ(seqs.size(), 10u);                // tail capped at 10 (B+C)
    EXPECT_EQ(std::get<0>(seqs.front()), 200u); // oldest survivor is 200
    EXPECT_EQ(std::get<0>(seqs.back()), 304u);
    EXPECT_TRUE(store.getTailEvents(sid, {chl::EventSequence{100, 1, 0}}).empty());

    // ...but A is still retained: not shipped, no watermark covers it.
    EXPECT_EQ(store.retainedChunkCount(sid), 3u);
}

// ---- the free condition: shipped AND W >= endTime AND tail released --------

TEST(KeeperChunkRetentionStore, FreeOrderShippedThenWatermarkThenTailRelease)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 10);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 10, 1, "A"));

    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr); // shipped
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);

    store.confirmPersisted(sid, 200);             // W reaches endTime
    EXPECT_EQ(store.retainedChunkCount(sid), 1u); // tail still references it

    // tail release: B evicts all of A's events
    store.ingestSealedChunk(sid, makeChunk(sid, 200, 300, 200, 10, 1, "B"));
    EXPECT_EQ(store.retainedChunkCount(sid), 1u); // A freed, B retained
    EXPECT_TRUE(store.getTailEvents(sid, {chl::EventSequence{100, 1, 0}}).empty());
}

TEST(KeeperChunkRetentionStore, FreeOrderWatermarkThenShippedThenTailRelease)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 10);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 10, 1, "A"));

    store.confirmPersisted(sid, 200); // W first (other keepers pushed it)
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);

    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr); // then shipped
    EXPECT_EQ(store.retainedChunkCount(sid), 1u); // tail still references it

    store.ingestSealedChunk(sid, makeChunk(sid, 200, 300, 200, 10, 1, "B"));
    EXPECT_EQ(store.retainedChunkCount(sid), 1u); // A freed, B retained
}

TEST(KeeperChunkRetentionStore, FreeOrderTailReleaseThenWatermarkThenShipped)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0); // capacity 0: tail releases at ingest
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 10, 1, "A"));
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);
    EXPECT_TRUE(store.getTailSequences(sid, 100).empty());

    store.confirmPersisted(sid, 200);
    EXPECT_EQ(store.retainedChunkCount(sid), 1u); // not shipped yet

    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr); // last condition
    EXPECT_EQ(store.retainedChunkCount(sid), 0u);
}

TEST(KeeperChunkRetentionStore, ConfirmBelowEndTimeFreesNothing)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 10, 1, "A"));
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr);

    store.confirmPersisted(sid, 199); // just below endTime
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);
    EXPECT_EQ(store.knownPersisted(sid), 199u);

    store.confirmPersisted(sid, 200);
    EXPECT_EQ(store.retainedChunkCount(sid), 0u);
}

TEST(KeeperChunkRetentionStore, TailReadsServeWhileShippedAwaitingW)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 5, 1, "A"));
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr); // SHIPPED_AWAITING_W

    auto seqs = store.getTailSequences(sid, 5);
    ASSERT_EQ(seqs.size(), 5u);
    auto evs = store.getTailEvents(sid, seqs);
    ASSERT_EQ(evs.size(), 5u);
    EXPECT_EQ(evs[0].getRecord(), "A#0");
}

TEST(KeeperChunkRetentionStore, MarkSendFailedKeepsChunkReadableAndResendable)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 5, 1, "A"));
    chl::StoryChunk* chunk = drainOne(q, store, /*transfer_ok=*/false); // transfer failed
    ASSERT_NE(chunk, nullptr);

    // still readable
    auto evs = store.getTailEvents(sid, store.getTailSequences(sid, 5));
    ASSERT_EQ(evs.size(), 5u);
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);

    // and eligible for re-send: stall timer re-stashes the same pointer
    EXPECT_EQ(store.requeueStalled(std::chrono::seconds(0)), 1u);
    EXPECT_EQ(q.size(), 1);
    EXPECT_EQ(q.ejectStoryChunk(), chunk);
    store.markSendFailed(chunk); // close the drain protocol so nothing dangles
}

TEST(KeeperChunkRetentionStore, RequeueStalledSkipsQueuedCoveredAndFreshChunks)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;

    // c1: drained, send failed -> stalled, eligible
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 5, 1, "A"));
    chl::StoryChunk* c1 = drainOne(q, store, /*transfer_ok=*/false);
    ASSERT_NE(c1, nullptr);

    // c2: drained, shipped, W covers it (still tail-indexed, so retained) -> not eligible
    store.ingestSealedChunk(sid, makeChunk(sid, 200, 300, 200, 5, 1, "B"));
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr);
    store.confirmPersisted(sid, 300);

    // c3: freshly sealed, its pointer still sits in the extraction queue -> must not double-stash
    store.ingestSealedChunk(sid, makeChunk(sid, 300, 400, 300, 5, 1, "C"));
    ASSERT_EQ(q.size(), 1);

    EXPECT_EQ(store.requeueStalled(std::chrono::seconds(0)), 1u); // only c1
    ASSERT_EQ(q.size(), 2);
    chl::StoryChunk* c3 = q.ejectStoryChunk();
    EXPECT_EQ(c3->getStartTime(), 300u); // c3, stashed at seal
    EXPECT_EQ(q.ejectStoryChunk(), c1);  // the re-send
    store.markSendFailed(c3); // close the drain protocol so nothing dangles
    store.markSendFailed(c1);
}

TEST(KeeperChunkRetentionStore, RequeueStalledHonorsMaxAge)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 5, 1, "A"));
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/false), nullptr);

    // chunk activity is recent; a 1-hour stall threshold must not re-send it
    EXPECT_EQ(store.requeueStalled(std::chrono::seconds(3600)), 0u);
    EXPECT_EQ(q.size(), 0);
}

TEST(KeeperChunkRetentionStore, WatermarkRegressionIsIgnored)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0); // tail releases at ingest
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 5, 1, "A"));
    chl::StoryChunk* c1 = drainOne(q, store, /*transfer_ok=*/false); // retained, unshipped
    ASSERT_NE(c1, nullptr);

    store.confirmPersisted(sid, 500);
    EXPECT_EQ(store.knownPersisted(sid), 500u);

    store.confirmPersisted(sid, 150); // regression: a lagging/restarted grapher
    EXPECT_EQ(store.knownPersisted(sid), 500u);
    EXPECT_EQ(store.retainedChunkCount(sid), 1u); // still held: never shipped

    // the kept (high) watermark still frees once the ack finally lands
    EXPECT_EQ(store.requeueStalled(std::chrono::seconds(0)), 1u);
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr);
    EXPECT_EQ(store.retainedChunkCount(sid), 0u);
}

// ---- destructor: owned chunks are not leaked and not double-freed ----------

TEST(KeeperChunkRetentionStore, DestructorReleasesRetainedChunks)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    {
        chl::KeeperChunkRetentionStore store(q, 100);
        chl::StoryId sid = 7;
        // one chunk still queued (in_queue), one drained+failed, one shipped
        store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 5, 1, "A"));
        store.ingestSealedChunk(sid, makeChunk(sid, 200, 300, 200, 5, 1, "B"));
        store.ingestSealedChunk(sid, makeChunk(sid, 300, 400, 300, 5, 1, "C"));
        ASSERT_NE(drainOne(q, store, /*transfer_ok=*/false), nullptr); // A: send failed
        ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr);  // B: shipped
        ASSERT_EQ(q.size(), 1);                                        // C still queued
    }
    // The store forwarded A (unshipped, not queued) for a last-chance archive
    // attempt, left C to the queue that already holds it, and freed B (shipped)
    // itself. The queue now owns what remains (A and C); its own destructor
    // deletes them at end of scope. ASan/valgrind verifies: no leak of A/B/C,
    // no double-free.
    EXPECT_EQ(q.size(), 2);
}
