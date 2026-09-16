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

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
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

// Simulate one full drain iteration for the oldest stashed chunk: eject the
// pointer from the queue and deliver the transfer outcome to the store.
static chl::StoryChunk*
drainOne(chl::StoryChunkExtractionQueue& q, chl::KeeperChunkRetentionStore& store, bool transfer_ok)
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
    EXPECT_EQ(q.size(), 1);                                                  // ship-on-seal
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
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);                 // tail still references it

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

// ---- shutdown flush --------------------------------------------------------

// A chunk whose transfer failed sits unshipped until the stall timer re-sends it,
// and watermark_resend_timeout_secs defaults to 720s -- so a keeper shutting down
// inside that window still holds it. The destructor has a last-chance stash for
// exactly this, but it runs after shutdownExtraction(), by which point the queue
// has been drained and joined and its own shutdown merely frees what is left. That
// data was silently lost. flushUnshippedChunks() hands it over while extraction can
// still drain it.
TEST(KeeperChunkRetentionStore, FlushHandsUnshippedChunksOverWhileExtractionRuns)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 5, 1, "A"));
    chl::StoryChunk* chunk = drainOne(q, store, /*transfer_ok=*/false); // the send failed
    ASSERT_NE(chunk, nullptr);
    ASSERT_EQ(q.size(), 0) << "precondition: the chunk is waiting on the stall timer, not queued";

    EXPECT_EQ(store.flushUnshippedChunks(), 1u) << "an unshipped chunk was not handed over for archival";
    EXPECT_EQ(q.size(), 1);
    EXPECT_EQ(q.ejectStoryChunk(), chunk) << "the flushed pointer must be the retained chunk";
    store.markSendFailed(chunk); // close the drain protocol so nothing dangles
}

// The flush must not hand a chunk over twice. Both guards matter: a chunk already
// in the queue is owned by it, and a shipped chunk needs no archival -- and the
// destructor skips in_queue chunks on the same contract, so a double handoff here
// would become a double free there.
TEST(KeeperChunkRetentionStore, FlushSkipsQueuedAndShippedChunks)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;

    // ship-on-seal already put this one in the queue
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 5, 1, "A"));
    ASSERT_EQ(q.size(), 1);
    EXPECT_EQ(store.flushUnshippedChunks(), 0u) << "flushed a chunk the queue already owns";
    EXPECT_EQ(q.size(), 1) << "the queue must not have gained a duplicate";

    chl::StoryChunk* chunk = drainOne(q, store, /*transfer_ok=*/true); // now shipped
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(store.flushUnshippedChunks(), 0u) << "flushed an already-shipped chunk";
    EXPECT_EQ(q.size(), 0);
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
    store.markSendFailed(c3);            // close the drain protocol so nothing dangles
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

// ---- fetchRange: the replay hot source -------------------------------------

TEST(KeeperChunkRetentionStore, FetchRangeSpansMultipleChunksAscending)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 5, 1, "A"));
    store.ingestSealedChunk(sid, makeChunk(sid, 200, 300, 200, 5, 1, "B"));
    store.ingestSealedChunk(sid, makeChunk(sid, 300, 400, 300, 5, 1, "C"));
    for(int i = 0; i < 3; ++i) { ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr); }

    auto response = store.fetchRange(sid, 102, 302, 1000);
    auto const& events = response.events;
    // [102, 302): A#2..A#4, all of B, C#0..C#1
    ASSERT_EQ(events.size(), 10u);
    EXPECT_EQ(events.front().time(), 102u);
    EXPECT_EQ(events.front().getRecord(), "A#2");
    EXPECT_EQ(events.back().time(), 301u);
    EXPECT_EQ(events.back().getRecord(), "C#1");
    for(std::size_t i = 1; i < events.size(); ++i) { EXPECT_LT(events[i - 1].time(), events[i].time()); }
    EXPECT_EQ(response.hot_floor, 100u); // oldest retained tick
    EXPECT_FALSE(response.truncated);
}

TEST(KeeperChunkRetentionStore, FetchRangeEmptyStoreReportsMaxFloor)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);

    auto response = store.fetchRange(42, 0, UINT64_MAX, 1000);
    EXPECT_TRUE(response.events.empty());
    EXPECT_EQ(response.hot_floor, UINT64_MAX); // nothing retained
    EXPECT_EQ(response.known_W, 0u);
    EXPECT_FALSE(response.truncated);
}

TEST(KeeperChunkRetentionStore, FetchRangeHonorsMaxEventsAndFlagsTruncation)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "A"));
    store.ingestSealedChunk(sid, makeChunk(sid, 200, 300, 200, 3, 1, "B"));
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr); // A acknowledged, B not sent yet

    // the cap counts both lists together and cuts the newest events, not the oldest
    auto response = store.fetchRange(sid, 100, 300, 4);
    ASSERT_EQ(response.events.size(), 3u);
    EXPECT_EQ(response.events.front().time(), 100u);
    ASSERT_EQ(response.unconfirmed_events.size(), 1u);
    EXPECT_EQ(response.unconfirmed_events.front().time(), 200u);
    EXPECT_TRUE(response.truncated);
}

TEST(KeeperChunkRetentionStore, FetchRangeServesTailEvictedChunks)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 5); // tail only indexes 5 events
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 5, 1, "A"));
    store.ingestSealedChunk(sid, makeChunk(sid, 200, 300, 200, 5, 1, "B")); // evicts A from the tail

    // A is out of the tail index but still retained (not shipped/covered):
    // its events may exist nowhere else, so fetchRange must serve them (as
    // unconfirmed, since neither chunk reached the grapher) and hot_floor
    // must account for them.
    EXPECT_TRUE(store.getTailEvents(sid, {chl::EventSequence{100, 1, 0}}).empty());

    auto response = store.fetchRange(sid, 0, 1000, 1000);
    EXPECT_TRUE(response.events.empty());
    ASSERT_EQ(response.unconfirmed_events.size(), 10u);
    EXPECT_EQ(response.unconfirmed_events.front().getRecord(), "A#0");
    EXPECT_EQ(response.hot_floor, 100u);
    EXPECT_FALSE(response.truncated);
}

TEST(KeeperChunkRetentionStore, FetchRangeFloorRisesAsChunksFree)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0); // tail releases at ingest
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 5, 1, "A"));
    store.ingestSealedChunk(sid, makeChunk(sid, 200, 300, 200, 5, 1, "B"));
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr); // A shipped
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr); // B shipped

    EXPECT_EQ(store.fetchRange(sid, 0, 1000, 1000).hot_floor, 100u);

    store.confirmPersisted(sid, 200); // frees A only
    auto response = store.fetchRange(sid, 0, 1000, 1000);
    ASSERT_EQ(response.events.size(), 5u);
    EXPECT_EQ(response.hot_floor, 200u); // floor rises with the free
    EXPECT_EQ(response.known_W, 200u);
}

// ---- receipts: chunks the grapher merged late -------------------------------
//
// The grapher acknowledges a chunk when it arrives and decides only later where
// its events go. A chunk that arrives after W already covers its range lands in
// a reopened past window or a salvage file, and neither moves W. So the ack
// carries a receipt, the grapher's report lists the receipts whose events are
// not written yet, and a chunk frees only once W covers it and its receipt is
// settled.

static chl::StoryWatermarkReport
watermarkReport(uint64_t w, uint64_t instance, uint64_t highest_receipt, std::vector<uint64_t> pending = {})
{
    chl::StoryWatermarkReport report;
    report.watermark = w;
    report.grapher_instance = instance;
    report.highest_receipt = highest_receipt;
    report.pending_receipts = std::move(pending);
    return report;
}

// Ship the oldest queued chunk under the receipt a grapher returned for it, as
// the RDMA extractor records it.
static void shipWithReceipt(chl::StoryChunkExtractionQueue& q,
                            chl::KeeperChunkRetentionStore& store,
                            uint64_t grapher_instance,
                            uint64_t receipt)
{
    chl::StoryChunk* chunk = q.ejectStoryChunk();
    ASSERT_NE(chunk, nullptr);
    chunk->setGrapherReceipt(grapher_instance, receipt);
    store.markShipped(chunk);
}

constexpr uint64_t kGrapher = 9;

TEST(KeeperChunkRetentionStore, ChunkAckedUnderACoveringWatermarkWaitsForItsReceipt)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.applyReport(sid, watermarkReport(300, kGrapher, 4));
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "late"));
    shipWithReceipt(q, store, kGrapher, 5);

    // acked and covered by W, but the grapher has not said its events are written
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);

    store.applyReport(sid, watermarkReport(300, kGrapher, 5, {5}));
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);

    store.applyReport(sid, watermarkReport(300, kGrapher, 5));
    EXPECT_EQ(store.retainedChunkCount(sid), 0u);
}

TEST(KeeperChunkRetentionStore, ReceiptAboveTheReportedHighestIsNotSettled)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "late"));
    shipWithReceipt(q, store, kGrapher, 5);

    // built before the grapher assigned receipt 5, delivered after the ack
    store.applyReport(sid, watermarkReport(300, kGrapher, 4));
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);
}

TEST(KeeperChunkRetentionStore, ReceiptFromAnotherGrapherInstanceIsNotSettled)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "late"));
    shipWithReceipt(q, store, kGrapher, 5);

    // the grapher restarted: its receipts start over and say nothing about ours
    store.applyReport(sid, watermarkReport(300, kGrapher + 1, 50));
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);
}

TEST(KeeperChunkRetentionStore, RestartedGrapherSettlesAResentChunkBelowTheKnownWatermark)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "resent"));
    shipWithReceipt(q, store, kGrapher, 5);
    store.applyReport(sid, watermarkReport(500, kGrapher, 5, {5}));
    ASSERT_EQ(store.retainedChunkCount(sid), 1u);

    // the grapher restarts before writing it, and the keeper sends it again
    ASSERT_EQ(store.requeueStalled(std::chrono::seconds(0)), 1u);
    shipWithReceipt(q, store, kGrapher + 1, 1);

    // the new instance's W starts below the one the keeper knows; its first
    // report has receipt 1 pending, a later one has it written
    store.applyReport(sid, watermarkReport(250, kGrapher + 1, 1, {1}));
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);
    store.applyReport(sid, watermarkReport(260, kGrapher + 1, 1));
    EXPECT_EQ(store.retainedChunkCount(sid), 0u);
    EXPECT_EQ(store.knownPersisted(sid), 500u);
}

TEST(KeeperChunkRetentionStore, DelayedReportFromTheSameGrapherDoesNotUndoANewerOne)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 10);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 10, 1, "A"));
    shipWithReceipt(q, store, kGrapher, 5);
    store.applyReport(sid, watermarkReport(500, kGrapher, 5)); // A settled; the tail still holds it

    // sent before the report above, with receipt 5 still pending
    store.applyReport(sid, watermarkReport(300, kGrapher, 5, {5}));
    // and one sent before receipt 5 was assigned
    store.applyReport(sid, watermarkReport(200, kGrapher, 4));

    store.ingestSealedChunk(sid, makeChunk(sid, 200, 300, 200, 10, 1, "B")); // A leaves the tail
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);                            // A freed, B queued
}

// ---- shutdown -----------------------------------------------------------------
//
// A keeper frees everything it holds when it exits. An acked chunk may still
// exist only in the grapher's memory, and a send can fail after the keeper
// handed the chunk to the extraction queue. So on SIGTERM the keeper sends again
// whatever is not acked and waits, with extraction and watermark reports still
// running, until the grapher has confirmed every chunk written.

TEST(KeeperChunkRetentionStore, ShutdownWaitEndsWhenTheGrapherConfirmsEveryChunk)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 10);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "A"));
    shipWithReceipt(q, store, kGrapher, 1); // acked, not written yet

    std::atomic<bool> reporting{false};
    std::thread grapher(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                reporting = true;
                store.applyReport(sid, watermarkReport(200, kGrapher, 1));
            });
    bool const confirmed =
            store.waitUntilDurable(std::chrono::seconds(5), std::chrono::milliseconds(10), std::chrono::seconds(30));
    bool const returned_after_report = reporting;
    grapher.join();

    EXPECT_TRUE(confirmed);
    EXPECT_TRUE(returned_after_report);
    // confirmed is enough: the tail may keep the chunk
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);
}

TEST(KeeperChunkRetentionStore, ShutdownWaitSendsAgainAChunkWhoseSendFails)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "A"));

    // the drain thread: the first send fails once the wait is under way, the
    // second is acked and the grapher confirms it
    std::thread drain(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                int attempts = 0;
                auto const give_up = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while(attempts < 2 && std::chrono::steady_clock::now() < give_up)
                {
                    chl::StoryChunk* chunk = q.ejectStoryChunk();
                    if(chunk == nullptr)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }
                    if(++attempts == 1)
                    {
                        store.markSendFailed(chunk);
                        continue;
                    }
                    chunk->setGrapherReceipt(kGrapher, 1);
                    store.markShipped(chunk);
                    store.applyReport(sid, watermarkReport(200, kGrapher, 1));
                }
            });
    bool const confirmed =
            store.waitUntilDurable(std::chrono::seconds(3), std::chrono::milliseconds(10), std::chrono::seconds(30));
    drain.join();

    EXPECT_TRUE(confirmed);
    EXPECT_EQ(store.retainedChunkCount(sid), 0u);
}

TEST(KeeperChunkRetentionStore, ShutdownWaitSendsAgainAChunkTheGrapherNeverWrote)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "A"));
    // acked, but the grapher's write failed: its receipt never settles, and no
    // report will ever confirm this delivery
    shipWithReceipt(q, store, kGrapher, 1);

    // the drain thread: the wait sends the chunk again, and this time the
    // grapher writes it and confirms
    std::thread drain(
            [&]
            {
                auto const give_up = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while(std::chrono::steady_clock::now() < give_up)
                {
                    chl::StoryChunk* chunk = q.ejectStoryChunk();
                    if(chunk == nullptr)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }
                    chunk->setGrapherReceipt(kGrapher, 2);
                    store.markShipped(chunk);
                    store.applyReport(sid, watermarkReport(200, kGrapher, 2));
                    return;
                }
            });
    // stall age 0: in production it is watermark_resend_timeout_secs, and only a
    // delivery that old is assumed lost
    bool const confirmed =
            store.waitUntilDurable(std::chrono::seconds(3), std::chrono::milliseconds(10), std::chrono::seconds(0));
    drain.join();

    EXPECT_TRUE(confirmed);
    EXPECT_EQ(store.retainedChunkCount(sid), 0u);
}

// The grapher issues a fresh receipt for every delivery and can only settle one
// a whole write window later. So a re-send throws away the receipt the keeper is
// waiting on and replaces it with a younger one that has to settle from scratch:
// re-sending faster than the grapher writes starves the wait, and every chunk
// stays unconfirmed however long the keeper waits.
TEST(KeeperChunkRetentionStore, ShutdownWaitLeavesARecentDeliveryAloneWhileItsReceiptSettles)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "A"));

    std::atomic<int> receipts_issued{0};
    std::atomic<bool> stop{false};
    // the grapher: acks every delivery under a new receipt and settles it one
    // write window (150ms here) later
    std::thread grapher(
            [&]
            {
                std::vector<std::pair<uint64_t, std::chrono::steady_clock::time_point>> writing;
                while(!stop)
                {
                    if(chl::StoryChunk* chunk = q.ejectStoryChunk(); chunk != nullptr)
                    {
                        uint64_t const receipt = (uint64_t)++receipts_issued;
                        chunk->setGrapherReceipt(kGrapher, receipt);
                        store.markShipped(chunk);
                        writing.emplace_back(receipt,
                                             std::chrono::steady_clock::now() + std::chrono::milliseconds(150));
                    }
                    auto const now = std::chrono::steady_clock::now();
                    for(auto it = writing.begin(); it != writing.end();)
                    {
                        if(now < it->second)
                        {
                            ++it;
                            continue;
                        }
                        store.applyReport(sid, watermarkReport(200, kGrapher, it->first));
                        it = writing.erase(it);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            });

    bool const confirmed =
            store.waitUntilDurable(std::chrono::seconds(3), std::chrono::milliseconds(10), std::chrono::seconds(30));
    stop = true;
    grapher.join();

    EXPECT_TRUE(confirmed);
    EXPECT_EQ(receipts_issued.load(), 1); // the one delivery, never sent again
}

// Same thing without the grapher: a chunk acked moments ago must not go back
// into the extraction queue on the next poll of the wait.
TEST(KeeperChunkRetentionStore, ShutdownWaitDoesNotResendADeliveryYoungerThanTheStallAge)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "A"));
    shipWithReceipt(q, store, kGrapher, 1); // acked; its receipt has not settled yet

    EXPECT_FALSE(store.waitUntilDurable(std::chrono::milliseconds(200),
                                        std::chrono::milliseconds(10),
                                        std::chrono::seconds(30)));
    // drains what the wait queued, if anything, so the store can free it
    EXPECT_EQ(drainOne(q, store, true), nullptr);
}

TEST(KeeperChunkRetentionStore, ShutdownWaitGivesUpAtTheTimeout)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "A"));
    shipWithReceipt(q, store, kGrapher, 1); // the grapher never confirms it

    auto const started = std::chrono::steady_clock::now();
    EXPECT_FALSE(store.waitUntilDurable(std::chrono::milliseconds(300),
                                        std::chrono::milliseconds(10),
                                        std::chrono::seconds(30)));
    auto const waited = std::chrono::steady_clock::now() - started;
    EXPECT_GE(waited, std::chrono::milliseconds(300));
    EXPECT_LT(waited, std::chrono::seconds(3));
}

TEST(KeeperChunkRetentionStore, ChunkShippedWithoutAReceiptFreesOnTheWatermarkAlone)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "local"));
    // an extractor that does not talk to a grapher returns no receipt
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr);

    store.applyReport(sid, watermarkReport(300, kGrapher, 50));
    EXPECT_EQ(store.retainedChunkCount(sid), 0u);
}

TEST(KeeperChunkRetentionStore, RequeueStalledResendsAnUnsettledChunkUnderTheWatermark)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.applyReport(sid, watermarkReport(300, kGrapher, 4));
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "late"));
    shipWithReceipt(q, store, kGrapher, 5);
    store.applyReport(sid, watermarkReport(300, kGrapher, 5, {5}));

    // covered by W but never confirmed written: the stall re-send must not skip it
    EXPECT_EQ(store.requeueStalled(std::chrono::seconds(0)), 1u);
    EXPECT_EQ(q.size(), 1);
}

TEST(KeeperChunkRetentionStore, FetchRangeCountsAnUnsettledChunkAsUnconfirmed)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::StoryId sid = 7;
    store.applyReport(sid, watermarkReport(300, kGrapher, 4));
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "late"));
    shipWithReceipt(q, store, kGrapher, 5);
    store.applyReport(sid, watermarkReport(300, kGrapher, 5, {5}));

    // below W, but not written: the archive cannot serve these yet
    auto response = store.fetchRange(sid, 0, 1000, 1000);
    EXPECT_TRUE(response.events.empty());
    EXPECT_EQ(response.unconfirmed_events.size(), 3u);
}

TEST(KeeperChunkRetentionStore, FetchRangeSeparatesEventsTheGrapherHasNotAcknowledged)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100);
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "A"));
    store.ingestSealedChunk(sid, makeChunk(sid, 200, 300, 200, 2, 1, "B"));
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr);  // A acknowledged
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/false), nullptr); // B's send failed

    // B never reached the grapher, so the archive cannot have it however far
    // the watermark moves: the player has to take its events from this keeper
    auto response = store.fetchRange(sid, 0, 1000, 1000);
    ASSERT_EQ(response.events.size(), 3u);
    EXPECT_EQ(response.events.front().getRecord(), "A#0");
    ASSERT_EQ(response.unconfirmed_events.size(), 2u);
    EXPECT_EQ(response.unconfirmed_events.front().getRecord(), "B#0");
    EXPECT_EQ(response.unconfirmed_events.back().getRecord(), "B#1");
    EXPECT_EQ(response.hot_floor, 100u);
}

// ---- archive visibility ------------------------------------------------------
//
// A report that a chunk is written does not mean a player can read it yet: a
// player finds new archive files only when it next scans the archive
// directory, and on a shared file system its listing can lag the grapher's
// write further. For the archive visibility delay after the keeper learns a
// chunk is written, the keeper keeps the chunk and serves its events as
// unconfirmed, so a replay takes them from the keeper.

TEST(KeeperChunkRetentionStore, WrittenChunkIsServedUnconfirmedUntilTheVisibilityDelayPasses)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100, 0, false, std::chrono::milliseconds(300));
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "A"));
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr);
    store.confirmPersisted(sid, 200);

    auto just_written = store.fetchRange(sid, 0, 1000, 1000);
    EXPECT_TRUE(just_written.events.empty());
    EXPECT_EQ(just_written.unconfirmed_events.size(), 3u);

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    auto visible = store.fetchRange(sid, 0, 1000, 1000);
    EXPECT_EQ(visible.events.size(), 3u);
    EXPECT_TRUE(visible.unconfirmed_events.empty());
}

TEST(KeeperChunkRetentionStore, VisibilityDelayStartsWhenTheChunkIsWritten)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100, 0, false, std::chrono::milliseconds(300));
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "A"));
    // sealed well before the grapher acknowledges it
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr);
    store.confirmPersisted(sid, 200);

    auto response = store.fetchRange(sid, 0, 1000, 1000);
    EXPECT_TRUE(response.events.empty());
    EXPECT_EQ(response.unconfirmed_events.size(), 3u);
}

TEST(KeeperChunkRetentionStore, DurableChunkIsFreedOnlyOnceTheVisibilityDelayHasPassed)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100, 0, false, std::chrono::milliseconds(300));
    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 100, 3, 1, "A"));
    ASSERT_NE(drainOne(q, store, /*transfer_ok=*/true), nullptr);
    store.confirmPersisted(sid, 200);
    store.releaseStoryTail(sid);

    // durable, but a player may not see the file yet
    EXPECT_EQ(store.freeDurableChunks(), 0u);
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);
    // waiting out the delay is not a stall
    EXPECT_EQ(store.requeueStalled(std::chrono::seconds(0)), 0u);
    EXPECT_EQ(q.size(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    // no further report comes for this chunk; the sweep frees it
    EXPECT_EQ(store.freeDurableChunks(), 1u);
    EXPECT_EQ(store.retainedChunkCount(sid), 0u);
}

// ---- concurrency: seal, drain, watermark, re-send and reads at once --------
//
// In the keeper these run on different threads against one store: the seal
// path ingests, the extraction streams deliver drain outcomes, the admin
// service applies watermark reports, the data-collection loop re-sends stalled
// chunks, and recording-service threads serve tail and range reads. A chunk
// freed by one path while another still holds it is a use-after-free or a
// double free. This drives all of them together, checks that every read sees
// intact events, and that the store drains down to the tail once W covers
// everything. Run under AddressSanitizer to turn a lifetime bug into a report.

TEST(KeeperChunkRetentionStore, ConcurrentSealDrainConfirmResendAndReads)
{
    ensureLogger();
    constexpr int kStories = 2;
    constexpr int kChunksPerStory = 300;
    constexpr int kEventsPerChunk = 8;
    constexpr std::size_t kTailCapacity = 16; // the last two chunks of a story
    constexpr uint64_t kSpan = 100;
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, kTailCapacity);

    std::atomic<bool> producing{true};
    std::atomic<bool> stop{false};
    std::atomic<int> read_errors{0};
    std::atomic<uint64_t> deliveries{0};
    std::atomic<uint64_t> sealed_end[kStories];
    for(auto& end: sealed_end) { end = 0; }

    // chunk i of a story spans [i*kSpan, (i+1)*kSpan); event e sits at
    // i*kSpan + e with record "c<i>#<e>", so a read can check each event
    auto checkEvent = [&](chl::LogEvent const& event)
    {
        uint64_t const time = event.time();
        if(event.getRecord() != "c" + std::to_string(time / kSpan) + "#" + std::to_string(time % kSpan))
        {
            read_errors++;
        }
    };

    std::vector<std::thread> writers;
    for(int s = 0; s < kStories; ++s)
    {
        writers.emplace_back(
                [&, s]
                {
                    chl::StoryId const sid = 100 + s;
                    for(int i = 0; i < kChunksPerStory; ++i)
                    {
                        uint64_t const start = kSpan * i;
                        store.ingestSealedChunk(sid,
                                                makeChunk(sid,
                                                          start,
                                                          start + kSpan,
                                                          start,
                                                          kEventsPerChunk,
                                                          1,
                                                          "c" + std::to_string(i)));
                        sealed_end[s] = start + kSpan;
                    }
                });
    }

    std::vector<std::thread> workers;
    for(int d = 0; d < 2; ++d)
    {
        workers.emplace_back(
                [&]
                {
                    while(!stop)
                    {
                        chl::StoryChunk* chunk = q.ejectStoryChunk();
                        if(chunk == nullptr)
                        {
                            std::this_thread::sleep_for(std::chrono::microseconds(50));
                            continue;
                        }
                        // a third of the deliveries fail while the writers run
                        if(producing && deliveries.fetch_add(1) % 3 == 0)
                        {
                            store.markSendFailed(chunk);
                        }
                        else
                        {
                            store.markShipped(chunk);
                        }
                    }
                });
    }
    workers.emplace_back(
            [&]
            {
                // W trails the seal point; every seventh report lags behind
                // what was already reported and must be ignored
                for(uint64_t report = 1; !stop; ++report)
                {
                    for(int s = 0; s < kStories; ++s)
                    {
                        uint64_t const w = sealed_end[s];
                        store.confirmPersisted(100 + s, (report % 7 == 0) ? w / 2 : w);
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            });
    workers.emplace_back(
            [&]
            {
                while(!stop)
                {
                    store.requeueStalled(std::chrono::seconds(0));
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
            });
    for(int r = 0; r < 2; ++r)
    {
        workers.emplace_back(
                [&, r]
                {
                    chl::StoryId const sid = 100 + r % kStories;
                    while(!stop)
                    {
                        for(auto const& event: store.getTailEvents(sid, store.getTailSequences(sid, kTailCapacity)))
                        {
                            checkEvent(event);
                        }
                        auto const response = store.fetchRange(sid, 0, UINT64_MAX, 100000);
                        auto const& events = response.events;
                        for(std::size_t i = 0; i < events.size(); ++i)
                        {
                            checkEvent(events[i]);
                            if(i > 0 && events[i - 1].time() >= events[i].time())
                            {
                                read_errors++;
                            }
                        }
                        if(!events.empty() && response.hot_floor > events.front().time())
                        {
                            read_errors++;
                        }
                    }
                });
    }

    for(auto& writer: writers) { writer.join(); }
    producing = false;

    // failed sends are re-sent and shipped, W covers every sealed chunk, and
    // all but the chunks the tail still indexes get freed
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool drained = false;
    while(!drained && std::chrono::steady_clock::now() < deadline)
    {
        drained = true;
        for(int s = 0; s < kStories; ++s)
        {
            drained = drained && store.retainedChunkCount(100 + s) == kTailCapacity / kEventsPerChunk;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop = true;
    for(auto& worker: workers) { worker.join(); }
    // hand back anything a last re-send left in the queue
    while(chl::StoryChunk* chunk = q.ejectStoryChunk()) { store.markShipped(chunk); }

    EXPECT_TRUE(drained);
    EXPECT_EQ(read_errors, 0);
    for(int s = 0; s < kStories; ++s)
    {
        EXPECT_EQ(store.retainedChunkCount(100 + s), kTailCapacity / kEventsPerChunk);
        EXPECT_EQ(store.knownPersisted(100 + s), kSpan * kChunksPerStory);
    }
}
